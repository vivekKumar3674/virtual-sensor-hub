// client.cpp - monitor_client: command-line client for sensor_daemon.
//
//   monitor_client [--host H] [--port P] GET          one-shot command
//   monitor_client [--host H] [--port P] WATCH        poll GET once a second
//   monitor_client [--host H] [--port P]              interactive prompt
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int connectTo(const std::string& host, const std::string& port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "resolve error: " << gai_strerror(rc) << "\n";
        return -1;
    }
    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) std::cerr << "cannot connect to " << host << ":" << port << "\n";
    return fd;
}

static bool sendLine(int fd, const std::string& s) {
    std::string msg = s + "\n";
    size_t off = 0;
    while (off < msg.size()) {
        ssize_t n = ::send(fd, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Reads one '\n'-terminated line; `pending` keeps bytes read past the newline.
static bool readLine(int fd, std::string& pending, std::string& line) {
    for (;;) {
        size_t pos = pending.find('\n');
        if (pos != std::string::npos) {
            line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            return true;
        }
        char buf[256];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        pending.append(buf, static_cast<size_t>(n));
    }
}

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", port = "5050", cmd;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = argv[++i];
        else if (a == "--help") {
            std::cerr << "Usage: " << argv[0] << " [--host H] [--port P] [COMMAND ...]\n"
                      << "Commands: GET, STATUS, SET_THRESHOLD <C>, SET_INTERVAL <ms>, WATCH, QUIT\n";
            return 0;
        } else {
            if (!cmd.empty()) cmd += ' ';
            cmd += a;
        }
    }

    int fd = connectTo(host, port);
    if (fd < 0) return 1;
    std::string pending, reply;

    auto roundTrip = [&](const std::string& c) -> bool {
        if (!sendLine(fd, c) || !readLine(fd, pending, reply)) {
            std::cerr << "connection lost\n";
            return false;
        }
        std::cout << reply << std::endl;
        return true;
    };

    int exitCode = 0;
    if (cmd == "WATCH" || cmd == "watch") {
        while (roundTrip("GET")) sleep(1);
        exitCode = 1;
    } else if (!cmd.empty()) {
        if (!roundTrip(cmd)) exitCode = 1;
        else if (reply.rfind("ERR", 0) == 0) exitCode = 3;
    } else {
        std::string line;
        std::cout << "Connected. Commands: GET STATUS SET_THRESHOLD <C> SET_INTERVAL <ms> WATCH QUIT\n> " << std::flush;
        while (std::getline(std::cin, line)) {
            if (line == "WATCH" || line == "watch") {
                while (roundTrip("GET")) sleep(1);
                break;
            }
            if (!line.empty() && !roundTrip(line)) break;
            if (line == "QUIT" || line == "quit") break;
            std::cout << "> " << std::flush;
        }
    }
    ::close(fd);
    return exitCode;
}
