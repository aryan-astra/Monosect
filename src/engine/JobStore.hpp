// MonoSect — the job history.
//
// Every finished job is appended to a single newline-delimited JSON file under
// the user's application data folder.  That format was chosen deliberately:
//   * an append of one line cannot corrupt the jobs that are already there,
//   * a file that is truncated by a power loss loses at most the last job, which
//     the loader reports instead of hiding,
//   * it is readable and diffable by hand, which matters for a tool that promises
//     to be honest about what it did.
// The record holds metadata only: paths, sizes, durations, the plan and the
// verification outcome.  Never media content.  No network code touches it.
#pragma once

#include "core/Json.hpp"
#include "core/Result.hpp"
#include "core/SplitPlan.hpp"
#include "core/Verification.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace monosect::core {

struct JobRecord {
    std::string jobId;
    std::string startedAt;             // ISO 8601, local time
    std::string finishedAt;
    std::string finalState;            // JobState name
    std::string message;
    std::string sourcePath;
    ByteCount sourceBytes = 0;
    Rational sourceDuration;
    std::string outputDirectory;
    std::string constraintDescription;
    std::string modeDescription;
    std::string appVersion;
    std::string ffmpegVersion;
    std::string ffprobeVersion;
    std::string planJson;
    std::vector<SegmentOutcome> parts;
    std::vector<std::string> warnings;
    ByteCount largestPartBytes = 0;
    Rational longestPartDuration;
    double planningSeconds = 0.0;
    double executionSeconds = 0.0;
    double verificationSeconds = 0.0;
    int partsRewritten = 0;

    [[nodiscard]] std::string toJson() const;
    static JobRecord fromJson(const JsonValue& value);
};

class JobStore {
public:
    /// Default location: %LOCALAPPDATA%\MonoSect\jobs.jsonl on Windows,
    /// $XDG_DATA_HOME/monosect/jobs.jsonl (or ~/.local/share/...) elsewhere.
    [[nodiscard]] static JobStore defaultStore();

    explicit JobStore(std::filesystem::path file) : file_(std::move(file)) {}

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return file_; }

    /// Appends one record.  The line is written and flushed as a whole.
    [[nodiscard]] Status append(const JobRecord& record) const;

    /// Reads the newest `limit` records (newest first).
    [[nodiscard]] Result<std::vector<JobRecord>> loadRecent(int limit = 200) const;

    [[nodiscard]] Result<JobRecord> find(const std::string& jobId) const;

    /// Number of jobs whose record could not be parsed (reported, never hidden).
    [[nodiscard]] int damagedRecordCount() const noexcept { return damagedRecords_; }

private:
    std::filesystem::path file_;
    mutable int damagedRecords_ = 0;
};

/// ISO 8601 local timestamp, e.g. "2026-10-03T21:15:04".
[[nodiscard]] std::string currentTimestamp();

/// Random, non-identifying job id (hex).  No hardware information is used.
[[nodiscard]] std::string makeJobId();

} // namespace monosect::core
