#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace vsm {

class WatchFolder final {
public:
    using Callback = std::function<void(const std::filesystem::path&)>;

    WatchFolder();
    ~WatchFolder();

    void start(const std::filesystem::path& folder, Callback callback);
    void stop();

private:
    struct DebounceState {
        uintmax_t lastObservedSize = 0;
        uint64_t lastObservedTick = 0;
        uintmax_t lastEmittedSize = static_cast<uintmax_t>(-1);
    };

    std::jthread worker_;
    std::atomic<bool> running_{ false };

    std::mutex stateMutex_;
    std::filesystem::path folder_;
    Callback callback_;
    std::unordered_map<std::string, DebounceState> debounce_;

    void run(std::stop_token token);
};

}