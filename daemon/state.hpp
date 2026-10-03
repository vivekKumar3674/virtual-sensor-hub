// state.hpp - data shared between the daemon's threads.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#include "sensor.hpp"

// A reading plus the user-space time at which we received it.
struct Sample {
    Reading r{};
    std::chrono::system_clock::time_point ts{};
};

// Everything the threads share. Simple counters/flags are std::atomic;
// the multi-field `latest` sample is protected by the mutex `m`.
struct SharedState {
    std::mutex m;
    Sample latest;            // guarded by m
    bool haveData = false;    // guarded by m

    std::atomic<int> thresholdMc{35000};   // alert threshold, milli-degC
    std::atomic<unsigned long> readings{0};
    std::atomic<unsigned long> alerts{0};
    std::atomic<bool> inAlert{false};      // state machine: false=NORMAL
    std::atomic<bool> running{true};       // cleared on shutdown

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
};

inline std::string isoTime(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return buf;
}

// 25340 (milli-units) -> "25.34"
inline std::string fixed2(int milli) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", milli / 1000.0);
    return buf;
}
