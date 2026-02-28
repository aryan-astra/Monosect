#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace vsm {

class ThreadPool final {
public:
    explicit ThreadPool(std::size_t workers = std::thread::hardware_concurrency());
    ~ThreadPool();

    void enqueue(std::function<void()> task);

private:
    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
};

}