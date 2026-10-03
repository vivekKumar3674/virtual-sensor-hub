// fake_sensor.cpp - stands in for /dev/vsensor so the user-space daemon can be
// tested without loading the kernel module.
//
//   fake_sensor <fifo-path> [interval_ms=1000] [walk|ramp]
//
// It creates a FIFO (named pipe) and writes binary struct vsensor_reading
// records into it - exactly what the driver would return from read().
//   walk : temperature random-walks around 25 C
//   ramp : temperature rises 1 C per sample (deterministic, triggers alerts)
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vsensor_ioctl.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <fifo> [interval_ms] [walk|ramp]\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    unsigned interval = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1000;
    bool ramp = argc > 3 && std::string(argv[3]) == "ramp";

    signal(SIGPIPE, SIG_IGN);  // reader going away => write() fails with EPIPE
    if (mkfifo(path, 0600) < 0 && errno != EEXIST) {
        std::perror("mkfifo");
        return 1;
    }
    int fd = open(path, O_WRONLY);  // blocks until a reader (the daemon) opens
    if (fd < 0) {
        std::perror("open fifo");
        return 1;
    }

    vsensor_reading r{};
    r.temp_mc = ramp ? 30000 : 25000;
    r.hum_mpct = 50000;
    unsigned rng = 12345;
    for (;;) {
        r.seq++;
        if (ramp) {
            if (r.temp_mc < 60000) r.temp_mc += 1000;
        } else {
            rng = rng * 1664525u + 1013904223u;
            r.temp_mc += static_cast<int>((rng >> 16) % 1001) - 500;
        }
        if (write(fd, &r, sizeof(r)) != static_cast<ssize_t>(sizeof(r))) break;
        usleep(interval * 1000);
    }
    close(fd);
    unlink(path);
    return 0;
}
