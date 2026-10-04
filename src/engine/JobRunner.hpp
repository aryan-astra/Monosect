// MonoSect — the job runner.
//
// Lifecycle of a job, in order:
//   probe -> validate -> plan -> (per part: render, verify, publish) -> sequence
//   check -> job record.
//
// Two rules define the class:
//   * A part becomes visible under its final name only after it passed
//     verification; every failure path deletes the temporary file.
//   * A hard limit is never exceeded: when a part comes out too large, the plan
//     for that part and the parts after it is corrected with the measured value
//     and the part is written again, and if that cannot be done the job fails
//     instead of publishing something larger than the limit.
#pragma once

#include "core/JobState.hpp"
#include "core/SplitPlanner.hpp"
#include "engine/FFprobeService.hpp"
#include "engine/FfmpegArgs.hpp"
#include "engine/OutputCommitter.hpp"
#include "engine/ProcessRunner.hpp"
#include "engine/VerificationEngine.hpp"

#include <functional>
#include <string>
#include <vector>

namespace monosect::core {

struct JobRequest {
    std::string sourcePath;
    std::string outputDirectory;
    UserConstraint constraint;
    NamingPolicy naming;
    VerificationPolicy verification;
    PlannerSettings plannerSettings;

    int ffmpegThreads = 0;              // 0 = let FFmpeg choose
    bool lowPriority = false;
    int maximumAttemptsPerPart = 4;     // a part that verifies as too large is written again, smaller
    int closenessPasses = 1;            // extra renders to approach the size limit (0 = none)
    bool keepTemporaryFilesOnFailure = false;
    /// Where the job record is appended.  Empty means JobStore::defaultStore().
    /// A record is written for every job that reaches a terminal state, so the
    /// history survives a restart (TODO 336-344).  A failure to write it never
    /// fails the job itself.
    std::string databasePath;
    bool recordHistory = true;
    std::string jobTag = "job";
    std::string appVersion;
    std::string ffmpegVersion;
    std::string ffprobeVersion;
};

struct JobEvents {
    std::function<void(JobState, const std::string&)> onState;
    std::function<void(const std::string&)> onLog;
    std::function<void(const SplitPlan&)> onPlanReady;
    std::function<void(int partIndex, int partCount, double fraction, const std::string& stage)> onProgress;
    CancelCheck shouldCancel;
    CancelCheck shouldPause;
};

struct JobOutcome {
    bool succeeded = false;
    bool canceled = false;
    bool paused = false;
    JobState finalState = JobState::Failed;
    std::string message;
    std::string jobId;
    SplitPlan plan;
    std::vector<SegmentOutcome> parts;
    std::vector<std::filesystem::path> publishedFiles;
    std::vector<std::string> warnings;
    std::vector<std::string> log;
    double planningSeconds = 0.0;
    double executionSeconds = 0.0;
    double verificationSeconds = 0.0;
    int partsRewrittenAfterFailure = 0;
    int partsRewrittenForCloseness = 0;

    [[nodiscard]] ByteCount largestPublishedBytes() const;
    [[nodiscard]] Rational longestPublishedDuration() const;
    /// One-paragraph summary used by the CLI, the UI and the job record.
    [[nodiscard]] std::string summary() const;
};

class JobRunner {
public:
    JobRunner(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath);

    /// Plans without writing anything (used by "Preview plan" in the UI).
    [[nodiscard]] Result<SplitPlan> planOnly(const JobRequest& request, const JobEvents& events);

    [[nodiscard]] Result<JobOutcome> run(const JobRequest& request, const JobEvents& events);

private:
    /// The job itself.  `run` wraps it so that the job record is written on every
    /// exit path, including the failure ones.
    [[nodiscard]] Result<JobOutcome> runInternal(const JobRequest& request, const JobEvents& events);
    void recordJob(const JobRequest& request, const JobEvents& events, JobOutcome& outcome);
    void recordFailure(const JobRequest& request, const JobEvents& events, const Error& error);

    struct RenderResult {
        std::filesystem::path file;
        SegmentOutcome outcome;
    };

    [[nodiscard]] Result<RenderResult> renderPart(const SplitPlan& plan, const SegmentPlan& segment,
                                                  const std::filesystem::path& temporaryPath,
                                                  const JobRequest& request, const JobEvents& events);
    /// One stream of a range, as it has to be trimmed.
    struct StreamTrimSpec {
        int streamIndex = -1;
        StreamKind kind = StreamKind::Unknown;
        std::int64_t packetCount = -1;    // -1: no count reported by the plan
    };

    /// Copies one range of the source into a container without re-encoding.
    ///
    /// This is the exact copy path: one FFmpeg run per stream, started with the
    /// seek form that is exact for that stream and stopped by an exact packet
    /// count, followed by a mux that joins the trimmed streams.  See
    /// FfmpegArgs.hpp for the measurements that make each part of it necessary.
    ///
    /// `packetlessStreams` are the source's stream indexes that carry no
    /// time-dependent data - attachments and data streams.  They cannot be trimmed,
    /// because there is no range of them to trim, so they are mapped from the
    /// source in the mux step instead of being dropped.  See
    /// MuxStreamsRequest::packetlessStreams.
    [[nodiscard]] Result<std::filesystem::path> copyRange(const SplitPlan& plan,
                                                         const std::filesystem::path& outputPath,
                                                         Rational start, Rational end,
                                                         const std::vector<StreamTrimSpec>& trims,
                                                         const JobRequest& request, const JobEvents& events,
                                                         bool startIsKeyframe,
                                                         std::vector<std::string>* notes,
                                                         const std::vector<int>& packetlessStreams = {});

    /// Packet counts of one range per stream, taken from the probed index.
    [[nodiscard]] std::int64_t packetCountInRange(int streamIndex, Rational start, Rational end) const;

    /// Fills a render request with the source video stream's own pixel format,
    /// H.264 profile and frame rate, so a re-encoded stream is described the way the
    /// source is rather than the way FFmpeg's encoder defaults are.
    ///
    /// Without this, encoding a Constrained Baseline source with default x264
    /// settings produces a High profile, the finished part no longer matches the
    /// source's signature, and the verifier discards it - so every smart cut and
    /// every full re-encode of a baseline source failed.  Profiles are translated
    /// from ffprobe's spelling ("Constrained Baseline") to the encoder's
    /// ("constrained_baseline"); a profile the encoder would not recognise is
    /// dropped rather than passed on, since an unknown value is worse than none.
    void describeSourceVideo(SegmentRenderRequest* render) const;

    [[nodiscard]] Result<RenderResult> renderCopy(const SplitPlan& plan, const SegmentPlan& segment,
                                                  const std::filesystem::path& temporaryPath,
                                                  const JobRequest& request, const JobEvents& events);
    [[nodiscard]] Result<RenderResult> renderSmartCut(const SplitPlan& plan, const SegmentPlan& segment,
                                                      const std::filesystem::path& temporaryPath,
                                                      const JobRequest& request, const JobEvents& events);
    [[nodiscard]] Result<RenderResult> renderReencode(const SplitPlan& plan, const SegmentPlan& segment,
                                                      const std::filesystem::path& temporaryPath,
                                                      const JobRequest& request, const JobEvents& events);

    [[nodiscard]] Error interpretFfmpegFailure(const ProcessResult& result, const std::filesystem::path& output,
                                               const std::string& commandLine) const;

    FFprobeService& prober_;
    IProcessRunner& runner_;
    std::string ffmpegPath_;
    /// The snapshot of the source of the job that is currently running.  A job
    /// runner instance handles one job at a time; keeping the index here avoids
    /// re-reading it when the plan has to be corrected after a measurement.
    ProbeSnapshot source_;
    bool sourceValid_ = false;
    /// Identity of the running job, kept here so that a job record written by
    /// `run` (which is outside `runInternal`) carries the same values.
    std::string currentJobId_;
    std::string jobStartedAt_;
};

} // namespace monosect::core
