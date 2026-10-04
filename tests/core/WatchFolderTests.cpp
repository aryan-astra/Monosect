// MonoSect — watch-folder readiness rules (TODO 208 - TODO 214).
//
// These are the tests that would have caught the original defect: a file copied
// in one operation produced a single notification and was never ingested,
// because the old rule needed to observe the same size twice before it would
// accept anything.  The rule under test now has three independent signals, and
// the tests pin down which one is missing in each situation - because that is the
// information the user is shown when they ask why a file has not been picked up.
#include "TestSupport.hpp"

#include "core/WatchReadiness.hpp"

using namespace monosect::core;
using namespace monosect::test;

namespace {

WatchCandidate candidate(const char* name, std::uintmax_t size, bool exclusive, bool readable) {
    WatchCandidate value;
    value.path = std::filesystem::path(name);
    value.size = size;
    value.exclusiveOpenSucceeded = exclusive;
    value.readable = readable;
    return value;
}

} // namespace

MONOSECT_TEST(a_finished_file_is_ready_after_the_settling_interval) {
    ReadinessHistory history;
    ReadinessSettings settings;
    settings.settlingSeconds = 5.0;

    // First observation: the file has been seen but nothing has been stable yet.
    ReadinessDecision decision =
        decideReadiness(candidate("a.mkv", 4 * 1024 * 1024, true, true), history, 1000, settings);
    CHECK(decision.verdict == ReadinessVerdict::NotYet);
    CHECK(decision.reason.find("settling") != std::string::npos);

    // The producer holds the file open: exclusivity fails and that is the reason.
    decision = decideReadiness(candidate("a.mkv", 4 * 1024 * 1024, false, true), history, 3000, settings);
    CHECK(decision.verdict == ReadinessVerdict::NotYet);
    CHECK(decision.reason.find("still has the file open") != std::string::npos);

    // Quiet, exclusive, readable, past the interval -> ready, exactly once.
    decision = decideReadiness(candidate("a.mkv", 4 * 1024 * 1024, true, true), history, 7000, settings);
    CHECK(decision.verdict == ReadinessVerdict::Ready);
    CHECK(decision.newlyReady);

    // A second look must not announce it again, or a job would run twice.
    decision = decideReadiness(candidate("a.mkv", 4 * 1024 * 1024, true, true), history, 9000, settings);
    CHECK(decision.verdict == ReadinessVerdict::NotYet);
    CHECK(decision.reason.find("already announced") != std::string::npos);
}

MONOSECT_TEST(a_single_notification_is_enough_because_the_rule_is_re_evaluated) {
    // The regression this file exists for.  The old implementation required two
    // observations of the same size *and* more than a second between them, so a
    // file copied in one shot - one notification, stable immediately - was never
    // ingested.  Here a single observation plus the passage of the settling
    // interval is enough, because the rules are re-evaluated on a timer rather
    // than driven by the count of notifications.
    ReadinessHistory history;
    ReadinessSettings settings;
    settings.settlingSeconds = 2.0;

    // One observation of a file that is already exclusive, readable and complete.
    const ReadinessDecision first =
        decideReadiness(candidate("copy.mkv", 4 * 1024 * 1024, true, true), history, 0, settings);
    CHECK(first.verdict == ReadinessVerdict::NotYet);

    // The watcher keeps looking on its timer; no further *event* is needed.
    const ReadinessDecision second =
        decideReadiness(candidate("copy.mkv", 4 * 1024 * 1024, true, true), history, 2500, settings);
    CHECK(second.verdict == ReadinessVerdict::Ready);
    CHECK(second.newlyReady);
}

MONOSECT_TEST(a_growing_file_resets_the_settling_clock) {
    ReadinessHistory history;
    ReadinessSettings settings;
    settings.settlingSeconds = 5.0;

    CHECK(decideReadiness(candidate("b.mp4", 1024 * 1024, true, true), history, 0, settings)
              .verdict == ReadinessVerdict::NotYet);
    // Still growing at t=4000: the size changed, so the clock restarts.
    CHECK(decideReadiness(candidate("b.mp4", 2 * 1024 * 1024, true, true), history, 4000, settings)
              .verdict == ReadinessVerdict::NotYet);
    // Growing again at t=6000 - four seconds after the last change, which would
    // have been enough had the size not moved.
    CHECK(decideReadiness(candidate("b.mp4", 3 * 1024 * 1024, true, true), history, 6000, settings)
              .verdict == ReadinessVerdict::NotYet);
    // Now it stops.
    const ReadinessDecision decision =
        decideReadiness(candidate("b.mp4", 3 * 1024 * 1024, true, true), history, 12000, settings);
    CHECK(decision.verdict == ReadinessVerdict::Ready);
}

MONOSECT_TEST(a_stalled_producer_is_not_mistaken_for_a_finished_one) {
    // The case the size-only rule got wrong in the other direction: a writer that
    // opened the file, wrote a header and then went quiet for a minute.  The size
    // has not changed and the settling interval has long passed, but nothing else
    // has it open either... which means it *is* finished as far as Windows can
    // tell, and MonoSect says exactly that rather than inventing a liveness test.
    // What it must never do is the opposite: accept a file while a writer holds
    // it, no matter how long the size has been unchanged.
    ReadinessHistory history;
    ReadinessSettings settings;
    settings.settlingSeconds = 1.0;
    for (int tick = 0; tick < 20; ++tick) {
        const ReadinessDecision decision =
            decideReadiness(candidate("c.mkv", 8 * 1024 * 1024, /*exclusive=*/false, true), history,
                            static_cast<std::uint64_t>(tick) * 1000, settings);
        CHECK(decision.verdict == ReadinessVerdict::NotYet);
        CHECK(decision.reason.find("still has the file open") != std::string::npos);
    }
}

MONOSECT_TEST(empty_and_tiny_files_are_never_announced) {
    ReadinessHistory history;
    ReadinessSettings settings;
    settings.settlingSeconds = 0.0;
    CHECK(decideReadiness(candidate("d.mp4", 0, true, false), history, 10000, settings)
              .reason.find("empty") != std::string::npos);
    CHECK(decideReadiness(candidate("e.mp4", 10, true, true), history, 10000, settings)
              .reason.find("floor") != std::string::npos);
}

MONOSECT_TEST(a_non_media_file_is_rejected_and_not_waited_on_forever) {
    ReadinessHistory history;
    ReadinessSettings settings;
    settings.settlingSeconds = 0.0;
    const ReadinessDecision decision =
        decideReadiness(candidate("notes.txt", 4 * 1024 * 1024, true, true), history, 10000, settings);
    CHECK(decision.verdict == ReadinessVerdict::Rejected);
    CHECK(decision.reason.find("media file") != std::string::npos);
    // Rejection is immediate and does not depend on the settling interval.
    CHECK(decision.reason.find("settling") == std::string::npos);
}

MONOSECT_TEST(the_extension_list_covers_the_capability_matrix) {
    for (const char* extension : {"mkv", "mka", "webm", "mp4", "m4v", "mov", "m4a", "ts", "m2ts", "avi",
                                  "flv", "ogg", "ogv", "wav", "mp3", "flac", "MP4", "MKV", "TS"}) {
        const std::string path = std::string("movie.") + extension;
        CHECK(isWatchableMediaExtension(path));
    }
    for (const char* extension : {"txt", "srt", "nfo", "jpg", "partial", "tmp", "log", "part"}) {
        CHECK(!isWatchableMediaExtension(std::string("file.") + extension));
    }
    // A dot in a directory name is not an extension.
    CHECK(!isWatchableMediaExtension("folder.mp4/notes.txt"));
    CHECK(!isWatchableMediaExtension("no-extension-at-all"));
    // The list is sorted and free of duplicates, which the binary search relies on.
    const std::vector<std::string>& all = watchableMediaExtensions();
    CHECK(std::is_sorted(all.begin(), all.end()));
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
}
