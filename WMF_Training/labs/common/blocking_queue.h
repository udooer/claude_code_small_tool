// blocking_queue.h — thread 之間傳遞資料用的簡單佇列（Lab 6）
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

template <typename T>
class BlockingQueue {
public:
    void Push(T item)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    // 有資料回傳 true；逾時或佇列已關閉且清空回傳 false
    bool Pop(T& out, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, timeout, [&] { return !q_.empty() || closed_; })) return false;
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return q_.size();
    }

    // 喚醒所有等待中的 Pop（結束 thread 用）
    void Close()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }
    bool Closed() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return closed_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<T> q_;
    bool closed_ = false;
};
