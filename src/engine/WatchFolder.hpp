// MonoSect — the watch folder (TODO 208 - TODO 214).
//
// What this file is, and why it is not the trivial `ReadDirectoryChangesW` loop
// it looks like it could be:
//
//   * **Shutdown must not block.**  The previous implementation called
//     `ReadDirectoryChangesW` with `lpOverlapped == nullptr`, which is a
//     *synchronous* call: it returns only when something happens in the folder.
//     A stop token cannot interrupt a blocked system call, so quitting
//     MonoSect while the watcher was idle hung the application until the next
//     file appeared - and on a disconnected network share, forever.  Here the
//     directory handle is opened with `FILE_FLAG_OVERLAPPED` and the wait is on
//     an event, so a stop request wakes it immediately and `CancelIoEx` tears
//     down the pending operation.
//
//   * **A notification can be lost.**  The kernel's per-handle notification
//     buffer is finite.  A burst of copies - exactly the case a watch folder
//     exists for - overflows it, and the overflow is reported as *no
//     information* (a zero-length result) rather than as a list.  A zero-length
//     result therefore triggers a full directory rescan instead of being
//     ignored, which is what the previous implementation did.
//
//   * **One notification is not one file event.**  A single copy produces
//     FILE_ACTION_ADDED followed by several FILE_ACTION_MODIFIED.  The
//     readiness rules (core/WatchReadiness.hpp) are re-evaluated on a timer, so
//     how many notifications arrived does not decide whether a file is ingested.
//
//   * **Events are only a hint.**  A periodic rescan guarantees that a file
//     which was somehow missed still gets picked up.  The watcher is a
//     convenience, and it is written so that losing every notification costs
//     latency, not correctness.
#pragma once

#include "core/WatchReadiness.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace monosect::core {

/// Statistics the UI shows, so a watcher that is doing nothing is
/// distinguishable from a watcher that is not running.
struct WatchFolderStats {
    bool running = false;
    std::string folder;
    std::uint64_t notifications = 0;
    std::uint64_t rescans = 0;
    std::uint64_t bufferOverflows = 0;
    std::uint64_t candidatesSeen = 0;
    std::uint64_t filesAccepted = 0;
    std::uint64_t rejected = 0;
    std::string lastError;
};

class WatchFolder {
public:
    using Callback = std::function<void(const std::filesystem::path&)>;

    WatchFolder() = default;
    ~WatchFolder();

    WatchFolder(const WatchFolder&) = delete;
    WatchFolder& operator=(const WatchFolder&) = delete;

    /// Starts watching `folder`.  Replaces an existing watch.  Returns a message
    /// describing why it could not start, or an empty string on success.
    [[nodiscard]] std::string start(const std::filesystem::path& folder, Callback callback,
                                    ReadinessSettings settings = {});

    /// Stops watching.  Returns promptly even when nothing is happening in the
    /// folder - that is the property the previous implementation did not have.
    /// Safe to call when not running, and safe to call twice.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] WatchFolderStats stats() const;
    /// Files that were seen and are still waiting for their settling interval,
    /// with the reason each one is waiting.  This is what makes the watcher
    /// explainable instead of magical: a user can ask why a file has not been
    /// picked up and get the real reason.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> pending() const;

    /// How long a file's size must be unchanged before it may be accepted.
    void setSettlingSeconds(double seconds);

private:
    void run();
    void rescanDirectory();
    /// Registers a path as a candidate, unless it is already known or is not a
    /// media file.
    void addCandidate(const std::filesystem::path& path);
    void evaluate(std::uint64_t nowTicks);
    [[nodiscard]] WatchCandidate observe(const std::filesystem::path& path) const;
    [[nodiscard]] std::uint64_t nowTicks() const;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Callback callback_;
    std::filesystem::path folder_;
    ReadinessSettings settings_{};

    /// What is remembered about one file between observations, plus the reason it
    /// has not been accepted yet.  Keeping the reason next to the state is what
    /// makes `pending()` able to answer "why has this file not been picked up".
    struct PendingEntry {
        ReadinessHistory history;
        std::string reason = "not examined yet";
    };
    std::map<std::string, PendingEntry> pending_;
    WatchFolderStats stats_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread worker_;
};

} // namespace monosect::core
