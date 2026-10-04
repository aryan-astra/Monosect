// MonoSect — the merge runner.
//
// Lifecycle: probe every input -> plan -> write a unique FFconcat manifest ->
// concatenate with `-c copy` into a temporary file -> verify the result ->
// publish it under its final name.
//
// The same rules as splitting apply: nothing becomes visible before it passed
// verification, the temporary file is deleted on every failure path, and a
// merge that cannot be done by copying is refused rather than "fixed" by
// silently re-encoding the user's media.
#pragma once

#include "core/MergePlanner.hpp"
#include "engine/FFprobeService.hpp"
#include "engine/JobRunner.hpp"   // JobEvents, JobOutcome-style reporting
#include "engine/OutputCommitter.hpp"
#include "engine/ProcessRunner.hpp"
#include "engine/VerificationEngine.hpp"

#include <string>
#include <vector>

namespace monosect::core {

struct MergeJobRequest {
    std::vector<std::string> inputs;     // in the user's order
    std::string outputDirectory;
    std::string outputFileName;          // resolved by the naming policy
    VerificationPolicy verification;
    bool overwriteExisting = false;
    bool lowPriority = false;
    int threads = 0;
    bool keepTemporaryFilesOnFailure = false;
    std::string databasePath;            // empty = JobStore::defaultStore()
    bool recordHistory = true;
    std::string jobTag = "merge";
    std::string appVersion;
    std::string ffmpegVersion;
    std::string ffprobeVersion;
};

struct MergeOutcome {
    bool succeeded = false;
    bool canceled = false;
    JobState finalState = JobState::Failed;
    std::string message;
    std::string jobId;
    MergePlan plan;
    std::string path;
    ByteCount fileSizeBytes = 0;
    Rational duration;
    bool durationMeasured = false;
    bool verified = false;
    ByteCount expectedPayloadBytes = 0;
    std::vector<std::string> findings;
    std::vector<std::string> warnings;
    std::vector<std::string> log;
    double seconds = 0.0;

    [[nodiscard]] std::string summary() const;
};

class MergeRunner {
public:
    MergeRunner(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath);

    /// Probes and plans without writing anything.
    [[nodiscard]] Result<MergePlan> planOnly(const MergeJobRequest& request, const JobEvents& events);

    [[nodiscard]] Result<MergeOutcome> run(const MergeJobRequest& request, const JobEvents& events);

private:
    [[nodiscard]] Result<MergeOutcome> runInternal(const MergeJobRequest& request, const JobEvents& events);
    void recordJob(const MergeJobRequest& request, const JobEvents& events, MergeOutcome& outcome);
    void recordFailure(const MergeJobRequest& request, const JobEvents& events, const Error& error);

    FFprobeService& prober_;
    IProcessRunner& runner_;
    std::string ffmpegPath_;
    // Set by runInternal so that a failure can still be recorded with the id and
    // the start time the user saw in the log.
    std::string currentJobId_;
    std::string jobStartedAt_;
};

} // namespace monosect::core
