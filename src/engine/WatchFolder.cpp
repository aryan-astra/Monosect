#include "engine/WatchFolder.hpp"
#include "engine/Types.hpp"

#include <windows.h>

#include <chrono>
#include <system_error>
#include <thread>
#include <vector>

namespace vsm {

WatchFolder::WatchFolder() = default;
WatchFolder::~WatchFolder() { stop(); }

void WatchFolder::start(const std::filesystem::path& folder, Callback callback) {
    stop();
    {
        std::lock_guard lock(stateMutex_);
        folder_ = folder;
        callback_ = std::move(callback);
    }
    running_.store(true);
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

void WatchFolder::stop() {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void WatchFolder::run(std::stop_token token) {
    std::filesystem::path folder;
    Callback callback;
    {
        std::lock_guard lock(stateMutex_);
        folder = folder_;
        callback = callback_;
    }

    if (folder.empty()) {
        return;
    }

    if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder)) {
        return;
    }

    HANDLE dir = CreateFileW(
        folder.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if (dir == INVALID_HANDLE_VALUE) {
        return;
    }

    std::vector<char> buffer(64 * 1024);

    while (!token.stop_requested() && running_.load()) {
        DWORD bytesReturned = 0;
        const BOOL ok = ReadDirectoryChangesW(
            dir,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesReturned,
            nullptr,
            nullptr);

        if (!ok || bytesReturned == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        FILE_NOTIFY_INFORMATION* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
        while (info != nullptr) {
            std::wstring relative(info->FileName, info->FileNameLength / sizeof(WCHAR));
            auto fullPath = folder / relative;

            std::error_code pathError;
            if (std::filesystem::is_regular_file(fullPath, pathError) && !pathError && isSupportedMediaExtension(fullPath)) {
                const auto key = fullPath.string();
                const auto now = static_cast<uint64_t>(GetTickCount64());
                uintmax_t size = 0;
                std::error_code ec;
                size = std::filesystem::file_size(fullPath, ec);

                if (!ec) {
                    auto& state = debounce_[key];

                    if (state.lastObservedSize == size && (now - state.lastObservedTick) > 1000) {
                        if (state.lastEmittedSize != size && callback) {
                            callback(fullPath);
                            state.lastEmittedSize = size;
                        }
                    } else {
                        state.lastEmittedSize = static_cast<uintmax_t>(-1);
                    }

                    state.lastObservedSize = size;
                    state.lastObservedTick = now;
                }
            }

            if (info->NextEntryOffset == 0) {
                break;
            }
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(info) + info->NextEntryOffset);
        }
    }

    CloseHandle(dir);
}

}