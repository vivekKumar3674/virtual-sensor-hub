// logger.hpp - append-only, thread-safe text log (RAII: the file is closed
// automatically when the Logger is destroyed).
#pragma once

#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

class Logger {
public:
    // Opens `path` for appending. If the file is new/empty, writes `header`
    // (e.g. CSV column names) first.
    explicit Logger(const std::string& path, const std::string& header = "") {
        bool empty;
        {
            std::ifstream probe(path);
            empty = !probe.good() || probe.peek() == std::ifstream::traits_type::eof();
        }
        out_.open(path, std::ios::app);
        if (!out_) throw std::runtime_error("cannot open log file: " + path);
        if (empty && !header.empty()) out_ << header << '\n' << std::flush;
    }

    void write(const std::string& line) {
        std::lock_guard<std::mutex> lk(m_);
        out_ << line << '\n' << std::flush;
    }

private:
    std::ofstream out_;
    std::mutex m_;
};
