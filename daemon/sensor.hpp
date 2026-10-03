// sensor.hpp - abstraction over "something that produces readings".
//
// Sensor is an abstract base class (pure virtual functions). DeviceSensor is
// the concrete implementation that talks to the kernel driver. Code that uses
// a Sensor does not care whether it is the real driver, a FIFO fed by the
// fake sensor in tests, or something else (polymorphism / DIP from SOLID).
#pragma once

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "vsensor_ioctl.h"

using Reading = vsensor_reading;

class Sensor {
public:
    virtual ~Sensor() = default;
    // Blocks until the next reading. Returns false on error / end of data;
    // errno describes the failure.
    virtual bool read(Reading& out) = 0;
    // Ask the sensor to change its sampling interval. false on failure.
    virtual bool setIntervalMs(unsigned ms) = 0;
};

// RAII wrapper around the file descriptor of the device: the constructor
// acquires it, the destructor always releases it, even if an exception is
// thrown somewhere else.
class DeviceSensor : public Sensor {
public:
    explicit DeviceSensor(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0)
            throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    }
    ~DeviceSensor() override {
        if (fd_ >= 0) ::close(fd_);
    }
    DeviceSensor(const DeviceSensor&) = delete;             // owns an fd:
    DeviceSensor& operator=(const DeviceSensor&) = delete;  // not copyable

    bool read(Reading& out) override {
        for (;;) {
            ssize_t n = ::read(fd_, &out, sizeof(out));
            if (n == static_cast<ssize_t>(sizeof(out))) return true;
            if (n < 0 && errno == EINTR) continue;  // interrupted: retry
            if (n >= 0) errno = EIO;                // EOF or short read
            return false;
        }
    }

    bool setIntervalMs(unsigned ms) override {
        __u32 v = ms;
        return ::ioctl(fd_, VSENSOR_IOC_SET_INTERVAL, &v) == 0;
    }

private:
    int fd_ = -1;
};
