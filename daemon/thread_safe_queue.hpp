// thread_safe_queue.hpp - blocking multi-producer / multi-consumer queue.
//
// A class template, so it works for any element type. pop() sleeps on a
// condition variable (no busy-waiting) until an item arrives or the queue is
// closed. After close(), consumers still drain the remaining items; pop()
// returns false only when the queue is closed AND empty.
#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class ThreadSafeQueue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (closed_) return;
            q_.push(std::move(value));
        }
        cv_.notify_one();
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;  // closed and drained
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<T> q_;
    bool closed_ = false;
};
