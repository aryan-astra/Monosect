#include "engine/ThreadPool.hpp"

namespace vsm {

ThreadPool::ThreadPool(std::size_t workers) {
    if (workers == 0) {
        workers = 1;
    }

    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this](std::stop_token token) {
            while (!token.stop_requested()) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, token, [this]() {
                        return !tasks_.empty();
                    });

                    if (token.stop_requested() && tasks_.empty()) {
                        return;
                    }
                    if (tasks_.empty()) {
                        continue;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    // jthread destructor requests stop and joins automatically;
    // wake all threads so they can observe the stop request.
    cv_.notify_all();
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

}