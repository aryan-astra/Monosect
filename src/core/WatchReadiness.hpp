// MonoSect — watch-folder readiness rules (TODO 208 - TODO 214).
//
// "Is this file finished?" is the whole problem in watching a folder, and it has
// no single correct answer that a size check can give.  The previous
// implementation required the same size to be observed twice, which meant a file
// copied in a single operation - the normal case, and the reason anyone watches
// a folder - produced one notification and was never ingested.  A size that has
// not grown for a second, on the other hand, proves nothing at all: a slow
// encoder can leave a partial file untouched for minutes.
//
// The rules below are the three independent signals the decision is built from,
// and they are deliberately separated from the Win32 notification code so that
// they can be unit-tested on any platform (tests/core/WatchFolderTests.cpp):
//
//   * **Exclusivity** - the file can be opened with no sharing at all.  A writer
//     that is still copying holds the file open, so this fails; a finished file
//     can be opened exclusively.  This is the only signal that says something
//     about the *producer* rather than about the clock.
//   * **Stability** - the size has not changed for a configurable settling
//     interval.  This is a floor on the wait, not a proof of completion; it is
//     kept because a producer can open, size, and still be flushing.
//   * **Readability** - the first bytes are readable, which rules out a file
//     that exists but has no content yet.
//
// All three must hold.  A file that never satisfies them is *not* rejected: it
// stays under observation, because a slow producer is normal and silently
// dropping its file would be worse than waiting.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace monosect::core {

/// The observations the decision is made from, gathered by the platform layer.
struct WatchCandidate {
    std::filesystem::path path;
    std::uintmax_t size = 0;
    /// True when the file could be opened with exclusive access (no sharing).
    /// Meaning: no other process holds it open, which is what a finished
    /// download or encode looks like.
    bool exclusiveOpenSucceeded = false;
    /// True when at least the first bytes could be read.
    bool readable = false;
    /// Windows error / errno text from the failed probe, for the log.
    std::string openFailureReason;
};

enum class ReadinessVerdict {
    /// Keep watching; the file is still being written or is not settled yet.
    NotYet,
    /// The producer is finished with this file and MonoSect may process it.
    Ready,
    /// This is not a file MonoSect will ever process (not a media extension,
    /// not a regular file, ...).  It is dropped from the watch set for good.
    Rejected,
};

struct ReadinessDecision {
    ReadinessVerdict verdict = ReadinessVerdict::NotYet;
    /// Always filled in: which of the signals is missing, or why it was dropped.
    std::string reason;
    /// True when this call is what made the file Ready, i.e. the caller should
    /// announce it exactly once.
    bool newlyReady = false;
};

/// What is remembered about one file between observations.
struct ReadinessHistory {
    bool seen = false;
    std::uintmax_t lastSize = 0;
    /// Monotonic clock reading (GetTickCount64 / steady_clock) of the last time
    /// the size *changed*.  Not of the last observation: that is the difference
    /// between "this file has been quiet" and "we looked again just now".
    std::uint64_t lastSizeChangeTicks = 0;
    /// True once the file has been announced, so it is never announced twice.
    bool announced = false;
    /// Tick reading of the first observation, used to reject "0 bytes forever".
    std::uint64_t firstSeenTicks = 0;
};

/// Settings for the readiness decision.
struct ReadinessSettings {
    /// How long the size must have been unchanged before a file may be accepted.
    double settlingSeconds = 5.0;
    /// A file smaller than this is never ready - it is a container that has not
    /// been given its payload yet.
    std::uintmax_t minimumBytes = 64 * 1024;
};

/// The decision, as a pure function.  `nowTicks` is a monotonic reading in
/// milliseconds; `history` is updated in place so the caller keeps it.
[[nodiscard]] ReadinessDecision decideReadiness(const WatchCandidate& candidate, ReadinessHistory& history,
                                                std::uint64_t nowTicks, const ReadinessSettings& settings);

/// The file extensions MonoSect will pick up from a watched folder.
///
/// Derived from the capability matrix (TODO 107): an extension is listed when a
/// container family that MonoSect can read has it.  Kept here, next to the
/// watcher, because the watcher is the only place that needs the list and a
/// second definition somewhere else is exactly how the two drift apart.
[[nodiscard]] bool isWatchableMediaExtension(const std::filesystem::path& path);

/// Every extension `isWatchableMediaExtension` accepts, lower case, no dot.
[[nodiscard]] const std::vector<std::string>& watchableMediaExtensions();

} // namespace monosect::core
