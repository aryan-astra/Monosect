#include "engine/WatchFolder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
// The watcher is a Windows feature.  On other platforms the class still exists so
// that the code that drives it compiles, but it reports that it cannot start
// rather than pretending to watch something.
#endif

namespace monosect::core {
namespace {

/// How often pending files are re-examined regardless of notifications.
constexpr std::uint64_t kTickIntervalMillis = 500;
/// Full directory rescan interval: the safety net for a lost notification.
constexpr std::uint64_t kRescanIntervalMillis = 30000;
/// The kernel's per-handle notification buffer.  64 KiB is the documented
/// default; a burst larger than this is detected through a zero-length result
/// and recovered by a rescan.
constexpr DWORD kNotifyBufferBytes = 64 * 1024;

std::uint64_t monotonicMillis() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

WatchFolder::~WatchFolder() {
    stop();
}

std::string WatchFolder::start(const std::filesystem::path& folder, Callback callback,
                               ReadinessSettings settings) {
    stop();
    if (!callback) {
        return "The watch folder needs a callback to deliver files to.";
    }
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error) || error) {
        return "The watch folder does not exist or is not a folder: " + folder.string();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
        folder_ = folder;
        settings_ = settings;
        pending_.clear();
        stats_ = WatchFolderStats{};
        stats_.folder = folder.string();
    }
    stopRequested_.store(false);
    running_.store(true);
    worker_ = std::thread([this]() { run(); });
    return {};
}

void WatchFolder::stop() {
    if (!worker_.joinable()) {
        running_.store(false);
        return;
    }
    stopRequested_.store(true);
    wake_.notify_all();
    // The wait inside run() is on an event, so this join returns as soon as the
    // worker observes the request.  The timeout is a backstop for a bug in the
    // wake-up path: a detached thread is worse than a late join, and a late join
    // is better than an application that cannot close.
    worker_.join();
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        stats_.running = false;
    }
}

WatchFolderStats WatchFolder::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void WatchFolder::setSettlingSeconds(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.settlingSeconds = seconds > 0.0 ? seconds : 1.0;
}

std::vector<std::pair<std::string, std::string>> WatchFolder::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(pending_.size());
    for (const auto& entry : pending_) {
        if (entry.second.history.announced) {
            continue;
        }
        out.emplace_back(entry.first, entry.second.reason);
    }
    return out;
}

std::uint64_t WatchFolder::nowTicks() const {
    return monotonicMillis();
}

WatchCandidate WatchFolder::observe(const std::filesystem::path& path) const {
    WatchCandidate candidate;
    candidate.path = path;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return candidate;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return candidate;
    }
    candidate.size = size;

#if defined(_WIN32)
    // Exclusivity: dwShareMode == 0 means "no other process may have this file
    // open at all".  A writer that is still copying holds a handle, so this
    // fails with ERROR_SHARING_VIOLATION - which is the single most reliable
    // "still being written" signal available on Windows, and far better than a
    // size comparison against a clock.
    HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        candidate.exclusiveOpenSucceeded = true;
        // Readability: the handle being open is not enough, the payload has to be
        // there.  One byte is enough to prove the container has been written to.
        unsigned char probe = 0;
        DWORD read = 0;
        if (::ReadFile(handle, &probe, 1, &read, nullptr) && read == 1) {
            candidate.readable = true;
        } else {
            candidate.readable = candidate.size > 0;
        }
        ::CloseHandle(handle);
    } else {
        const DWORD code = ::GetLastError();
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "Windows error %lu", static_cast<unsigned long>(code));
        candidate.openFailureReason = buffer;
        if (code == ERROR_SHARING_VIOLATION) {
            candidate.openFailureReason = "in use by another program";
        } else if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            candidate.openFailureReason = "it disappeared";
        } else if (code == ERROR_ACCESS_DENIED) {
            candidate.openFailureReason = "access denied";
        }
        // Still readable for our purposes if we can open it for reading with
        // sharing; the file may exist with a share mode that forbids our probe.
        std::ifstream stream(path, std::ios::binary);
        candidate.readable = stream.good();
    }
#else
    std::ifstream stream(path, std::ios::binary);
    candidate.readable = stream.good();
    // No exclusive-open primitive without platform code; a successful read is
    // the strongest signal this platform offers.
    candidate.exclusiveOpenSucceeded = candidate.readable;
#endif
    return candidate;
}

void WatchFolder::evaluate(std::uint64_t nowTicks) {
    std::vector<std::filesystem::path> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        candidates.reserve(pending_.size());
        for (const auto& entry : pending_) {
            candidates.emplace_back(entry.first);
        }
    }

    for (const std::filesystem::path& path : candidates) {
        const WatchCandidate candidate = observe(path);
        std::filesystem::path accepted;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::string key = path.string();
            PendingEntry& entry = pending_[key];
            const ReadinessDecision decision =
                decideReadiness(candidate, entry.history, nowTicks, settings_);
            entry.reason = decision.reason;
            if (decision.verdict == ReadinessVerdict::Rejected) {
                ++stats_.rejected;
                pending_.erase(key);
            } else if (decision.verdict == ReadinessVerdict::Ready && decision.newlyReady) {
                ++stats_.filesAccepted;
                accepted = path;
            }
        }
        if (!accepted.empty()) {
            Callback callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                callback = callback_;
            }
            if (callback) {
                callback(accepted);
            }
        }
    }
}

void WatchFolder::rescanDirectory() {
    std::vector<std::filesystem::path> found;
    std::error_code error;
    std::filesystem::directory_iterator it(folder_, std::filesystem::directory_options::skip_permission_denied,
                                           error);
    if (error) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.lastError = "the watch folder could not be read: " + error.message();
        return;
    }
    for (const auto& entry : it) {
        found.push_back(entry.path());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.rescans;
    }
    for (const std::filesystem::path& path : found) {
        addCandidate(path);
    }
}

void WatchFolder::addCandidate(const std::filesystem::path& path) {
    if (!isWatchableMediaExtension(path)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = path.string();
    if (pending_.find(key) == pending_.end()) {
        pending_[key] = PendingEntry{};
    }
    ++stats_.candidatesSeen;
}

#if defined(_WIN32)

void WatchFolder::run() {
    const std::filesystem::path folder = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        return folder_;
    }();

    HANDLE directory = ::CreateFileW(folder.wstring().c_str(), FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.running = false;
        stats_.lastError = "the folder could not be opened for watching (Windows error " +
                           std::to_string(static_cast<unsigned long>(code)) + ")";
        running_.store(false);
        return;
    }

    HANDLE changeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE tickEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (changeEvent == nullptr || stopEvent == nullptr || tickEvent == nullptr) {
        if (changeEvent) ::CloseHandle(changeEvent);
        if (stopEvent) ::CloseHandle(stopEvent);
        if (tickEvent) ::CloseHandle(tickEvent);
        ::CloseHandle(directory);
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.running = false;
        stats_.lastError = "the watch folder could not create its wait handles";
        running_.store(false);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.running = true;
        stats_.lastError.clear();
    }

    std::vector<unsigned char> buffer(kNotifyBufferBytes);
    std::uint64_t lastRescan = monotonicMillis();
    OVERLAPPED overlapped{};
    overlapped.hEvent = changeEvent;

    auto arm = [&]() -> bool {
        ::ResetEvent(changeEvent);
        DWORD returned = 0;
        // The overlapped pointer and the completion event are what make this
        // non-blocking: the call returns immediately and the event fires when a
        // change arrives (or when the buffer overflowed).
        return ::ReadDirectoryChangesW(directory, buffer.data(), kNotifyBufferBytes, TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                           FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                                       &returned, &overlapped, nullptr) != FALSE;
    };

    if (!arm()) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.lastError = "the folder could not be watched (Windows error " +
                           std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
    }
    ::SetEvent(tickEvent);

    while (!stopRequested_.load()) {
        HANDLE handles[3] = {changeEvent, stopEvent, tickEvent};
        const DWORD waited = ::WaitForMultipleObjects(3, handles, FALSE, kTickIntervalMillis);
        const std::uint64_t now = monotonicMillis();

        if (stopRequested_.load()) {
            break;
        }
        if (waited == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            if (!::GetOverlappedResult(directory, &overlapped, &transferred, FALSE)) {
                const DWORD code = ::GetLastError();
                if (code == ERROR_OPERATION_ABORTED) {
                    break;   // CancelIoEx from stop()
                }
            }
            if (transferred == 0) {
                // The documented overflow signal: the kernel dropped a batch of
                // changes.  Recovering by rescanning is the whole point.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.bufferOverflows;
                }
                rescanDirectory();
            } else {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.notifications;
                }
                auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
                while (info != nullptr) {
                    const std::wstring relative(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    if (!relative.empty()) {
                        addCandidate(folder / std::filesystem::path(relative));
                    }
                    if (info->NextEntryOffset == 0) {
                        break;
                    }
                    info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<unsigned char*>(info) +
                                                                       info->NextEntryOffset);
                }
            }
            if (!arm()) {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.lastError = "the watch subscription was lost (Windows error " +
                                   std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
                break;
            }
        } else if (waited == WAIT_TIMEOUT || waited == WAIT_OBJECT_0 + 2) {
            // Nothing happened.  Re-examine whatever is pending and, every
            // kRescanIntervalMillis, look for something the notifications never
            // mentioned.
            ::ResetEvent(tickEvent);
            ::SetEvent(tickEvent);
        }
        // WAIT_OBJECT_0 + 1 (stopEvent) is covered by the stopRequested_ check.

        if (now - lastRescan >= kRescanIntervalMillis) {
            lastRescan = now;
            rescanDirectory();
        }
        evaluate(now);
    }

    // Tear the pending operation down before closing anything it may touch.
    ::CancelIoEx(directory, &overlapped);
    ::CloseHandle(directory);
    ::CloseHandle(changeEvent);
    ::CloseHandle(stopEvent);
    ::CloseHandle(tickEvent);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.running = false;
    }
    running_.store(false);
}

#else // !_WIN32

void WatchFolder::run() {
    // The watcher needs ReadDirectoryChangesW, which is Windows-only.  MonoSect
    // is a Windows product, so this is the only path that matters; the class is
    // still compiled elsewhere so that code driving it links, and it reports the
    // limitation instead of silently watching nothing.
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.running = false;
    stats_.lastError = "The watch folder needs Windows' directory notifications; this build is not a Windows build.";
    running_.store(false);
}

#endif

} // namespace monosect::core
