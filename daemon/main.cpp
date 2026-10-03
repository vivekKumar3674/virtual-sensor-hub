// main.cpp - sensor_daemon: reads the vsensor driver, logs, raises alerts and
// serves remote clients.
//
// Threads
//   poller  : blocks in read() on the device, publishes each sample
//   logger  : writes every sample to readings.csv
//   alerter : state machine NORMAL <-> ALERT, writes alerts.log
//   server  : TCP accept loop (+ one thread per connected client)
//   main    : waits for SIGINT/SIGTERM with sigwait(), then shuts down
//
// Signals: SIGINT/SIGTERM are blocked in ALL threads and consumed by main via
// sigwait(). That avoids async signal handlers entirely (handlers may only
// call a tiny set of functions; sigwait lets us use normal code).
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <exception>
#include <memory>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "logger.hpp"
#include "sensor.hpp"
#include "server.hpp"
#include "state.hpp"
#include "thread_safe_queue.hpp"

namespace {

constexpr int kHysteresisMc = 1000;  // leave ALERT only 1 degC below threshold

struct Config {
    std::string device = "/dev/vsensor";
    int port = 5050;
    double thresholdC = 35.0;
    std::string logDir = "logs";
    bool daemon = false;
    bool bindAll = false;
};

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --device <path>      sensor device        (default /dev/vsensor)\n"
              << "  --port <n>           TCP port             (default 5050)\n"
              << "  --threshold <degC>   alert threshold      (default 35.0)\n"
              << "  --log-dir <dir>      directory for logs   (default ./logs)\n"
              << "  --any                listen on all interfaces (default: localhost only)\n"
              << "  --daemon             run in the background\n"
              << "  --help\n";
}

bool parseArgs(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        try {
            if (a == "--device") { if (!next(c.device)) return false; }
            else if (a == "--port") { if (!next(v)) return false; c.port = std::stoi(v); }
            else if (a == "--threshold") { if (!next(v)) return false; c.thresholdC = std::stod(v); }
            else if (a == "--log-dir") { if (!next(c.logDir)) return false; }
            else if (a == "--any") c.bindAll = true;
            else if (a == "--daemon") c.daemon = true;
            else return false;  // includes --help
        } catch (const std::exception&) {
            return false;       // bad number
        }
    }
    return c.port > 0 && c.port < 65536;
}

// Classic double-fork daemonization.
void daemonize() {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); std::exit(1); }
    if (pid > 0) _exit(0);              // parent exits
    if (setsid() < 0) { perror("setsid"); std::exit(1); }  // new session, no tty
    pid = fork();
    if (pid < 0) { perror("fork"); std::exit(1); }
    if (pid > 0) _exit(0);              // 2nd fork: can never re-acquire a tty
    umask(027);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
    }
}

// ---- thread bodies ---------------------------------------------------------

void pollerLoop(Sensor& sensor, SharedState& st,
                ThreadSafeQueue<Sample>& logQ, ThreadSafeQueue<Sample>& alertQ) {
    Sample s;
    while (st.running) {
        if (!sensor.read(s.r)) {
            if (st.running) {
                std::fprintf(stderr, "[poller] read failed: %s - shutting down\n",
                             std::strerror(errno));
                st.running = false;
                kill(getpid(), SIGTERM);  // wake main's sigwait()
            }
            break;
        }
        s.ts = std::chrono::system_clock::now();
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.latest = s;
            st.haveData = true;
        }
        ++st.readings;
        logQ.push(s);
        alertQ.push(s);
    }
}

void loggerLoop(ThreadSafeQueue<Sample>& q, Logger& csv) {
    Sample s;
    while (q.pop(s)) {
        csv.write(isoTime(s.ts) + "," + std::to_string(s.r.seq) + "," +
                  fixed2(s.r.temp_mc) + "," + fixed2(s.r.hum_mpct));
    }
}

void alertLoop(SharedState& st, ThreadSafeQueue<Sample>& q, Logger& alertLog) {
    Sample s;
    bool inAlert = false;
    while (q.pop(s)) {
        int thr = st.thresholdMc.load();
        if (!inAlert && s.r.temp_mc > thr) {
            inAlert = true;
            st.inAlert = true;
            ++st.alerts;
            std::string msg = isoTime(s.ts) + " ALERT temp=" + fixed2(s.r.temp_mc) +
                              "C exceeds threshold " + fixed2(thr) + "C";
            alertLog.write(msg);
            std::fprintf(stderr, "%s\n", msg.c_str());
        } else if (inAlert && s.r.temp_mc <= thr - kHysteresisMc) {
            inAlert = false;
            st.inAlert = false;
            std::string msg = isoTime(s.ts) + " CLEARED temp=" + fixed2(s.r.temp_mc) + "C";
            alertLog.write(msg);
            std::fprintf(stderr, "%s\n", msg.c_str());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) {
        usage(argv[0]);
        return 2;
    }

    SharedState st;
    st.thresholdMc = static_cast<int>(std::lround(cfg.thresholdC * 1000.0));

    // Create everything that can fail BEFORE daemonizing, so errors are still
    // visible on the terminal.
    std::unique_ptr<DeviceSensor> sensor;
    std::unique_ptr<Logger> csvLog, alertLog;
    std::unique_ptr<Server> server;
    try {
        std::filesystem::create_directories(cfg.logDir);
        csvLog = std::make_unique<Logger>(cfg.logDir + "/readings.csv",
                                          "timestamp,seq,temp_c,humidity_pct");
        alertLog = std::make_unique<Logger>(cfg.logDir + "/alerts.log");
        sensor = std::make_unique<DeviceSensor>(cfg.device);
        server = std::make_unique<Server>(cfg.port, cfg.bindAll, st, *sensor);
    } catch (const std::exception& e) {
        std::cerr << "startup error: " << e.what() << "\n";
        return 1;
    }

    if (cfg.daemon) daemonize();

    // Block SIGINT/SIGTERM here, BEFORE creating threads: threads inherit the
    // signal mask, so only main's sigwait() will ever receive them.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    ThreadSafeQueue<Sample> logQ, alertQ;
    std::thread tLogger(loggerLoop, std::ref(logQ), std::ref(*csvLog));
    std::thread tAlert(alertLoop, std::ref(st), std::ref(alertQ), std::ref(*alertLog));
    std::thread tServer([&] { server->run(); });
    std::thread tPoller(pollerLoop, std::ref(*sensor), std::ref(st), std::ref(logQ),
                        std::ref(alertQ));

    if (!cfg.daemon)
        std::cerr << "sensor_daemon running (device " << cfg.device << ", port " << cfg.port
                  << "). Ctrl-C to stop.\n";

    int sig = 0;
    sigwait(&set, &sig);  // sleeps until Ctrl-C / kill
    if (!cfg.daemon) std::cerr << "\nsignal " << sig << " received, shutting down...\n";

    // Orderly shutdown: stop producers, let consumers drain, then join.
    st.running = false;
    tPoller.join();   // returns after the device delivers its next reading
    logQ.close();
    alertQ.close();
    tLogger.join();
    tAlert.join();
    tServer.join();   // notices `running == false` within 0.5 s
    return 0;
}
