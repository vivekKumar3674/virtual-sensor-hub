// server.hpp - small line-based TCP command server.
//
// Protocol (one command per line, one reply line per command):
//   GET                  -> OK seq=.. temp=.. hum=.. ts=..
//   STATUS               -> OK state=NORMAL|ALERT readings=.. alerts=.. ...
//   SET_THRESHOLD <degC> -> OK threshold=..
//   SET_INTERVAL <ms>    -> OK interval=..   (asks the driver via ioctl)
//   QUIT                 -> OK bye           (server closes the connection)
// Errors are reported as "ERR <reason>".
//
// The accept loop uses poll() with a timeout so it can notice `running`
// becoming false and shut down; every client is served by its own thread.
#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sensor.hpp"
#include "state.hpp"

class Server {
public:
    // bindAll=false: listen on 127.0.0.1 only (safe default; there is no
    // authentication). bindAll=true: listen on all interfaces.
    Server(int port, bool bindAll, SharedState& st, Sensor& sensor)
        : st_(st), sensor_(sensor) {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

        int yes = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(bindAll ? INADDR_ANY : INADDR_LOOPBACK);

        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(listenFd_, 8) < 0) {
            std::string msg = std::string("bind/listen on port ") + std::to_string(port) +
                              ": " + std::strerror(errno);
            ::close(listenFd_);
            throw std::runtime_error(msg);
        }
    }

    ~Server() {
        if (listenFd_ >= 0) ::close(listenFd_);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Runs until st.running becomes false; then joins all client threads.
    void run() {
        while (st_.running) {
            pollfd pfd{listenFd_, POLLIN, 0};
            int rc = ::poll(&pfd, 1, 500);  // wake up twice a second
            if (rc <= 0) continue;
            int cfd = ::accept(listenFd_, nullptr, nullptr);
            if (cfd < 0) continue;
            clients_.emplace_back(&Server::handleClient, this, cfd);
        }
        for (auto& t : clients_) t.join();
        clients_.clear();
    }

private:
    static void sendAll(int fd, const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return;  // client went away
            off += static_cast<size_t>(n);
        }
    }

    void handleClient(int fd) {
        timeval tv{0, 500000};  // recv() times out every 0.5 s so we can check `running`
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string buf;
        char tmp[256];
        bool quit = false;

        while (st_.running && !quit) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n == 0) break;  // client closed
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                break;
            }
            buf.append(tmp, static_cast<size_t>(n));
            if (buf.size() > 1024) {
                sendAll(fd, "ERR line too long\n");
                break;
            }
            size_t pos;
            while (!quit && (pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                sendAll(fd, handle(line, quit) + "\n");
            }
        }
        ::close(fd);
    }

    std::string handle(const std::string& line, bool& quit) {
        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (cmd.empty()) return "ERR empty command";

        char out[256];

        if (cmd == "GET") {
            std::lock_guard<std::mutex> lk(st_.m);
            if (!st_.haveData) return "ERR no data yet";
            const Sample& s = st_.latest;
            std::snprintf(out, sizeof(out), "OK seq=%u temp=%s hum=%s ts=%s", s.r.seq,
                          fixed2(s.r.temp_mc).c_str(), fixed2(s.r.hum_mpct).c_str(),
                          isoTime(s.ts).c_str());
            return out;
        }

        if (cmd == "STATUS") {
            auto up = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - st_.start).count();
            std::snprintf(out, sizeof(out),
                          "OK state=%s readings=%lu alerts=%lu threshold=%s uptime=%llds",
                          st_.inAlert ? "ALERT" : "NORMAL", st_.readings.load(),
                          st_.alerts.load(), fixed2(st_.thresholdMc).c_str(),
                          static_cast<long long>(up));
            return out;
        }

        if (cmd == "SET_THRESHOLD") {
            double v;
            if (!(is >> v) || v < -40.0 || v > 125.0)
                return "ERR usage: SET_THRESHOLD <-40..125 degC>";
            st_.thresholdMc = static_cast<int>(std::lround(v * 1000.0));
            return "OK threshold=" + fixed2(st_.thresholdMc);
        }

        if (cmd == "SET_INTERVAL") {
            unsigned long ms;
            if (!(is >> ms)) return "ERR usage: SET_INTERVAL <ms>";
            if (!sensor_.setIntervalMs(static_cast<unsigned>(ms)))
                return std::string("ERR set interval failed: ") + std::strerror(errno);
            return "OK interval=" + std::to_string(ms);
        }

        if (cmd == "QUIT") {
            quit = true;
            return "OK bye";
        }

        return "ERR unknown command";
    }

    int listenFd_ = -1;
    SharedState& st_;
    Sensor& sensor_;
    std::vector<std::thread> clients_;
};
