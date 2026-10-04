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

static int connectTo(const std::string& host, const std::string& port)
{
    addrinfo hints{};
    addrinfo* result = nullptr;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (status != 0) {
        std::cerr << "resolve error: " << gai_strerror(status) << "\n";
        return -1;
    }

    int socketFd = -1;

    for (addrinfo* current = result; current != nullptr;
         current = current->ai_next) {

        socketFd = ::socket(
            current->ai_family,
            current->ai_socktype,
            current->ai_protocol
        );

        if (socketFd < 0)
            continue;

        if (::connect(
                socketFd,
                current->ai_addr,
                current->ai_addrlen) == 0) {
            break;
        }

        ::close(socketFd);
        socketFd = -1;
    }

    freeaddrinfo(result);

    if (socketFd < 0) {
        std::cerr << "cannot connect to "
                  << host << ":" << port << "\n";
    }

    return socketFd;
}

// Send a complete line to the server.
static bool sendLine(int socketFd, const std::string& text)
{
    const std::string message = text + "\n";
    size_t bytesSent = 0;

    while (bytesSent < message.size()) {
        ssize_t count = ::send(
            socketFd,
            message.data() + bytesSent,
            message.size() - bytesSent,
            MSG_NOSIGNAL
        );

        if (count <= 0)
            return false;

        bytesSent += static_cast<size_t>(count);
    }

    return true;
}

// Read one newline-terminated line.
// Any bytes received after the newline are kept in pending.
static bool readLine(
    int socketFd,
    std::string& pending,
    std::string& line)
{
    while (true) {
        size_t newline = pending.find('\n');

        if (newline != std::string::npos) {
            line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            return true;
        }

        char buffer[256];

        ssize_t count = ::recv(
            socketFd,
            buffer,
            sizeof(buffer),
            0
        );

        if (count <= 0)
            return false;

        pending.append(buffer, static_cast<size_t>(count));
    }
}

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    std::string port = "5050";
    std::string command;

    // Parse command-line arguments.
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];

        if (argument == "--host" && i + 1 < argc) {
            host = argv[++i];
        }
        else if (argument == "--port" && i + 1 < argc) {
            port = argv[++i];
        }
        else if (argument == "--help") {
            std::cerr
                << "Usage: " << argv[0]
                << " [--host H] [--port P] [COMMAND ...]\n"
                << "Commands: GET, STATUS, SET_THRESHOLD <C>, "
                << "SET_INTERVAL <ms>, WATCH, QUIT\n";

            return 0;
        }
        else {
            if (!command.empty())
                command += ' ';

            command += argument;
        }
    }

    int socketFd = connectTo(host, port);
    if (socketFd < 0)
        return 1;

    std::string pending;
    std::string reply;

    // Send a command and wait for one response.
    auto roundTrip = [&](const std::string& request) -> bool {
        if (!sendLine(socketFd, request) ||
            !readLine(socketFd, pending, reply)) {

            std::cerr << "connection lost\n";
            return false;
        }

        std::cout << reply << std::endl;
        return true;
    };

    int exitCode = 0;

    // WATCH repeatedly sends GET once every second.
    if (command == "WATCH" || command == "watch") {
        while (roundTrip("GET"))
            sleep(1);

        exitCode = 1;
    }
    else if (!command.empty()) {
        // Run a single command supplied on the command line.
        if (!roundTrip(command)) {
            exitCode = 1;
        }
        else if (reply.rfind("ERR", 0) == 0) {
            exitCode = 3;
        }
    }
    else {
        // No command was supplied, so enter interactive mode.
        std::string line;

        std::cout
            << "Connected. Commands: GET STATUS "
            << "SET_THRESHOLD <C> SET_INTERVAL <ms> WATCH QUIT\n"
            << "> "
            << std::flush;

        while (std::getline(std::cin, line)) {
            if (line == "WATCH" || line == "watch") {
                while (roundTrip("GET"))
                    sleep(1);

                break;
            }

            if (!line.empty() && !roundTrip(line))
                break;

            if (line == "QUIT" || line == "quit")
                break;

            std::cout << "> " << std::flush;
        }
    }

    ::close(socketFd);
    return exitCode;
}
