// MonoSect — the window's state machine (TODO 272 - TODO 299).
//
// The window is split in two on purpose:
//
//   * `UiController` (this file) owns every decision: what is queued, whether the
//     constraint is valid, when a plan is previewed, when a job starts, what the
//     status says, how a cancel or a pause is requested, what the results are.
//     It contains no ImGui, no GLFW and no Windows headers, so it is unit-tested
//     on the development machine (tests/core/UiControllerTests.cpp) against a
//     scripted engine.
//   * `src/ui/MainWindow.cpp` only draws what the controller exposes.  A drawing
//     bug can look wrong; it cannot report a job as verified when it was not.
//
// The engine is an interface so tests can run the whole flow without FFmpeg.  The
// application binds it to FFprobeService + SplitPlanner + JobRunner
// (`EngineUiEngine` below).
#pragma once

#include "core/Constraints.hpp"
#include "core/MediaModel.hpp"
#include "core/MergePlanner.hpp"
#include "core/PacketIndex.hpp"
#include "core/SplitPlan.hpp"
#include "core/Verification.hpp"
#include "engine/JobRunner.hpp"
#include "engine/JobStore.hpp"
#include "engine/MergeRunner.hpp"
#include "monitor/HardwareCapabilities.hpp"
#include "monitor/SystemMonitor.hpp"
#include "ui/HelpContent.hpp"
#include "ui/UiModel.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace monosect::core {

struct ProbedSource {
    MediaModel model;
    PacketIndex index;
};

/// A merge: two or more inputs joined into one file without re-encoding.
///
/// Kept apart from the split session rather than folded into it, because the two
/// answer different questions.  A split is constrained by a hard limit and is
/// verified part by part; a merge has no limit at all - the inputs either match or
/// they do not - and is verified as one output.  Sharing one state machine between
/// them produced a session where a merge could leave a stale split plan on screen.
struct MergeSession {
    bool active = false;
    std::vector<std::string> inputs;      // in the order they will be joined
    std::string outputDirectory;
    std::string outputName;
    bool planned = false;
    std::string planSummary;             // what the plan says, in a sentence
    std::string planProblem;             // why it cannot be planned, when it cannot
    bool running = false;
    bool succeeded = false;
    bool canceled = false;
    std::string headline;                // what the status line says
    std::string message;                 // the outcome, in full
    std::string path;                    // where the merged file is
    ByteCount fileSizeBytes = 0;
    Rational duration;
    bool durationKnown = false;
    bool verified = false;
    std::vector<std::string> findings;
    std::vector<std::string> warnings;
    std::string jobId;
};

/// What the window needs from the engine.  Implemented by `EngineUiEngine` in the
/// application and by a scripted fake in the tests.
class UiEngine {
public:
    virtual ~UiEngine();
    /// Container probe + packet index.  Runs on the calling thread.
    virtual Result<ProbedSource> probe(const std::string& path) = 0;
    /// Plan without writing.  Runs on the calling thread.
    virtual Result<SplitPlan> plan(const JobRequest& request, const JobEvents& events) = 0;
    /// Execute.  Runs on the worker thread the controller starts.
    virtual Result<JobOutcome> run(const JobRequest& request, const JobEvents& events) = 0;
    /// Merges.  `run` runs on a worker thread; these do too, so the window stays
    /// responsive.  A window with no merge panel never calls them.
    virtual Result<MergePlan> planMerge(const MergeJobRequest&, const JobEvents&) {
        return Error{ErrorCode::UnsupportedOperation, "This build cannot merge files.", {}, {}};
    }
    virtual Result<MergeOutcome> runMerge(const MergeJobRequest&, const JobEvents&) {
        return Error{ErrorCode::UnsupportedOperation, "This build cannot merge files.", {}, {}};
    }
    /// Version strings recorded in the job history.  May be empty.
    virtual std::string ffmpegVersion() { return {}; }
    virtual std::string ffprobeVersion() { return {}; }
};

/// Binds the interface to the real engine.
class EngineUiEngine final : public UiEngine {
public:
    EngineUiEngine(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath);
    Result<ProbedSource> probe(const std::string& path) override;
    Result<SplitPlan> plan(const JobRequest& request, const JobEvents& events) override;
    Result<JobOutcome> run(const JobRequest& request, const JobEvents& events) override;
    Result<MergePlan> planMerge(const MergeJobRequest& request, const JobEvents& events) override;
    Result<MergeOutcome> runMerge(const MergeJobRequest& request, const JobEvents& events) override;
    std::string ffmpegVersion() override;
    std::string ffprobeVersion() override;

private:
    FFprobeService& prober_;
    IProcessRunner& runner_;
    std::string ffmpegPath_;
};

/// How the controller obtains the machine's hardware encoder capabilities.
///
/// This is an interface for the same reason the engine is: the controller is
/// unit-tested on a machine with no GPU, so a probe that actually launched FFmpeg
/// would make every resource-policy test depend on a video card.  The application
/// binds it to HardwareCapabilityProbe; the tests bind it to a scripted answer.
class HardwareCapabilitySource {
public:
    virtual ~HardwareCapabilitySource();
    /// Runs the probe.  Returns immediately; the answer is delivered by polling.
    /// Never throws and never blocks.
    virtual void startProbe() = 0;
    /// True while a probe is in flight.
    virtual bool probeRunning() const = 0;
    /// The most recent result.  `probed` is false until a probe has completed.
    [[nodiscard]] virtual HardwareCapabilities capabilities() const = 0;
};

/// One source in the queue.
struct QueueItem {
    int id = 0;
    std::string path;
    std::string displayName;
    std::string status;          // what the list shows under the name
    bool probed = false;
    ByteCount sizeBytes = 0;
    Rational duration;
    std::string summary;         // parameterSummary() of the first video stream
    int streamCount = 0;
    std::string problem;         // probe failure, if any
};

enum class SessionPhase {
    Empty,        // no source selected
    Probing,
    Ready,        // a source is probed and a constraint can be edited
    Planning,
    Planned,      // a preview is on screen and the job can start
    Running,
    Pausing,      // the user asked to pause; it takes effect at a part boundary
    Paused,
    Cancelling,
    Completed,
    Failed,
};

[[nodiscard]] const char* sessionPhaseName(SessionPhase phase) noexcept;

/// Everything the results panel shows.  Kept as one struct so the panel never
/// reads a field the controller has not filled in yet.
struct SessionSnapshot {
    SessionPhase phase = SessionPhase::Empty;
    std::string statusHeadline;
    std::string statusDetail;
    StatusTone tone = StatusTone::Neutral;
    JobState jobState = JobState::Idle;
    int partsDone = 0;
    int partsTotal = 0;
    double fraction = 0.0;
    ByteCount bytesWritten = 0;
    std::string stage;
    std::optional<SplitPlan> plan;
    std::vector<SegmentOutcome> parts;
    std::vector<std::string> log;
    std::vector<std::string> warnings;
    std::string jobId;
    bool verified = false;
    bool canceled = false;
    double elapsedSeconds = 0.0;
};

class UiController {
public:
    /// `hardware` may be null, in which case the controller reports "not probed"
    /// rather than "no hardware" - the two are different answers and a machine
    /// MonoSect has not asked yet is not a machine without a GPU.
    explicit UiController(UiEngine& engine, HardwareCapabilitySource* hardware = nullptr);

    // ---- the queue -------------------------------------------------------------------
    /// Adds a source and selects it.  Returns its queue id.
    int addSource(const std::string& path);
    void removeSource(int id);
    void clearSources();
    [[nodiscard]] const std::vector<QueueItem>& queue() const noexcept { return queue_; }
    [[nodiscard]] int selectedId() const noexcept { return selectedId_; }
    void select(int id);
    [[nodiscard]] const QueueItem* selected() const;

    // ---- probing ---------------------------------------------------------------------
    /// Probes the selected source on the calling thread.  Failures are shown in
    /// the queue item, not thrown.
    void probeSelected();

    // ---- the constraint --------------------------------------------------------------
    [[nodiscard]] UserConstraint& constraint() noexcept { return constraint_; }
    [[nodiscard]] const UserConstraint& constraint() const noexcept { return constraint_; }
    void setSizeLimitBytes(ByteCount bytes);
    void clearSizeLimit();
    void setDurationLimit(Rational seconds);
    void clearDurationLimit();
    void setPreservation(PreservationMode mode);
    void setPrecision(PrecisionPolicy policy);
    void setNamingPattern(const std::string& pattern);
    [[nodiscard]] const NamingPolicy& naming() const noexcept { return naming_; }
    void setVerificationPolicy(const VerificationPolicy& policy) { verification_ = policy; }
    [[nodiscard]] const VerificationPolicy& verificationPolicy() const noexcept { return verification_; }
    /// Validates the constraint against the selected source.  Rechecked before
    /// every plan and every start, so an impossible request cannot be started.
    [[nodiscard]] ConstraintReport validate() const;
    /// True when the user changed the constraint after a plan was previewed: the
    /// preview is then stale and is labelled as such rather than reused.
    [[nodiscard]] bool previewIsStale() const noexcept { return previewStale_; }

    // ---- planning --------------------------------------------------------------------
    /// Plans and stores the preview.  Requires a probed source and a valid
    /// constraint; on refusal the reason is kept in the snapshot.
    void previewPlan(const std::string& outputDirectory);
    void discardPreview();
    [[nodiscard]] const std::optional<SplitPlan>& preview() const noexcept { return preview_; }

    // ---- running ---------------------------------------------------------------------
    /// Starts the job on a worker thread.  Refuses (and says why) when the source
    /// is not probed, the constraint is not valid, or a job is already running.
    void start(const std::string& outputDirectory);
    void requestCancel();
    void requestPause();
    void requestResume();
    /// Applies the events the worker pushed.  Call once per frame; it never blocks.
    void poll();
    /// True while the worker thread is running.  A worker that has finished but
    /// has not been joined yet does not count as running, so a finished job never
    /// blocks the next one.
    [[nodiscard]] bool jobRunning() const noexcept {
        return worker_.joinable() && !workerFinished_.load();
    }
    /// Waits for the worker to finish.  Used when the window closes and by tests.
    void waitForJob();

    // ---- resources -------------------------------------------------------------------
    /// Samples the machine.  The graphs are drawn from these, and an unmeasurable
    /// metric stays "n/a" (TODO 206/207).
    [[nodiscard]] const ResourceSample& sampleResources();
    [[nodiscard]] const std::string& resourceAvailabilityNote() const noexcept {
        return monitor_.availabilityNote();
    }
    [[nodiscard]] ResourcePolicyInput resourceInput() const;

    // ---- hardware encoder capability ------------------------------------------------
    /// True until a hardware probe has run.  The resources panel says "not probed
    /// yet" rather than "no hardware encoder", because those are different claims
    /// and only the second one is supported by evidence.
    [[nodiscard]] bool hardwareProbed() const;
    [[nodiscard]] bool hardwareProbeRunning() const;
    /// One paragraph for the resources panel, saying what was checked.
    [[nodiscard]] std::string hardwareSummary() const;
    /// The encoders as the machine answered, for the panel's detail listing.
    [[nodiscard]] std::vector<EncoderProbe> hardwareEncoders() const;

    // ---- merging ---------------------------------------------------------------------
    /// Opens the merge panel and puts it in front of the split panel.  The two are
    /// separate sessions on purpose: a merge has no size or duration limit to edit,
    /// so sharing the constraint panel would show controls that cannot mean anything.
    void showMerge();
    void hideMerge();
    [[nodiscard]] bool mergeVisible() const noexcept { return merge_.active; }
    [[nodiscard]] MergeSession& merge() noexcept { return merge_; }
    [[nodiscard]] const MergeSession& merge() const noexcept { return merge_; }

    /// Adds a file to the merge, at the end.  Probes nothing: the merge plan needs
    /// every input's codec parameters, and it reads them itself, so a file that
    /// cannot be read is reported by the plan rather than here.
    void addMergeInput(const std::string& path);
    void removeMergeInput(std::size_t index);
    void clearMergeInputs();
    /// Moves an input up or down.  The order is the join order and is not cosmetic:
    /// a concatenation plays in the order given.
    void moveMergeInput(std::size_t index, int delta);
    /// Plans the merge without writing.  Refuses (and says why) with fewer than two
    /// inputs, or while a split job is running.
    void previewMerge(const std::string& outputDirectory, const std::string& outputName);
    /// Runs the merge on a worker thread.  Refuses (and says why) when there is no
    /// valid plan, or a job of either kind is already running.
    void startMerge();
    void requestMergeCancel();
    [[nodiscard]] bool mergeRunning() const noexcept {
        return mergeWorker_.joinable() && !mergeWorkerFinished_.load();
    }
    /// Waits for a running merge to finish and joins its worker.  Used on shutdown
    /// and by the tests.
    void waitForMerge();

    // ---- job history -----------------------------------------------------------------
    void setDatabasePath(const std::string& path);
    void refreshHistory();
    [[nodiscard]] const std::vector<JobRecord>& history() const noexcept { return history_; }
    [[nodiscard]] const std::string& historyProblem() const noexcept { return historyProblem_; }

    // ---- help ------------------------------------------------------------------------
    void showHelp(HelpTopicId id);
    void closeHelp();
    [[nodiscard]] bool helpVisible() const noexcept { return helpVisible_; }
    [[nodiscard]] const HelpTopic* helpTopic() const noexcept { return helpTopic_; }
    void setHelpQuery(const std::string& query);
    [[nodiscard]] const std::string& helpQuery() const noexcept { return helpQuery_; }
    [[nodiscard]] const std::vector<const HelpTopic*>& helpResults() const noexcept { return helpResults_; }

    // ---- settings --------------------------------------------------------------------
    /// The planner settings the UI exposes (calibration renders etc.).
    [[nodiscard]] PlannerSettings& plannerSettings() noexcept { return plannerSettings_; }
    [[nodiscard]] const std::string& outputDirectory() const noexcept { return outputDirectory_; }
    void setOutputDirectory(const std::string& directory) { outputDirectory_ = directory; }
    [[nodiscard]] int ffmpegThreads() const noexcept { return ffmpegThreads_; }
    void setFfmpegThreads(int threads) { ffmpegThreads_ = threads; }
    [[nodiscard]] bool lowPriority() const noexcept { return lowPriority_; }
    void setLowPriority(bool value) { lowPriority_ = value; }
    /// The user's own choice about GPU use.  Kept apart from the probe's answer
    /// because "the card can encode" and "the user wants it to" are separate
    /// decisions, and the resource policy needs both.
    [[nodiscard]] bool gpuEnabled() const noexcept { return userEnabledGpu_; }
    void setGpuEnabled(bool value) { userEnabledGpu_ = value; }
    [[nodiscard]] bool hardwareDecodingEnabled() const noexcept { return userEnabledHardwareDecoding_; }
    void setHardwareDecodingEnabled(bool value) { userEnabledHardwareDecoding_ = value; }
    /// True when the current selection would need a re-encoded boundary, and so is
    /// the only case where a GPU encoder could matter at all.
    [[nodiscard]] bool jobWillEncode() const noexcept;

    // ---- what the panels read --------------------------------------------------------
    [[nodiscard]] const SessionSnapshot& snapshot() const noexcept { return snapshot_; }
    /// The whole status line as the header shows it.
    [[nodiscard]] JobStatusLine statusLine() const;
    [[nodiscard]] ProgressView progressView() const;
    /// The parts as the results table shows them, including the ones not written
    /// yet (from the preview) - an unwritten part is shown as planned, not as done.
    [[nodiscard]] std::vector<UiModel::PartLine> partLines() const;
    /// Verification summary of the last job, or the reason there is none.
    [[nodiscard]] JobStatusLine verificationLine() const;

private:
    struct PendingEvent {
        enum class Kind { State, Log, Plan, Progress } kind = Kind::Log;
        JobState state = JobState::Idle;
        std::string text;
        SplitPlan plan;
        int partIndex = 0;
        int partCount = 0;
        double fraction = 0.0;
        std::string stage;
    };

    void pushLog(const std::string& line);
    /// Joins the worker thread when it has already finished.  Never blocks on a
    /// running job.
    void reapFinishedWorker();
    /// Joins a finished merge worker.  Never blocks on a running one.
    void reapFinishedMergeWorker();
    void invalidatePreview();
    [[nodiscard]] JobRequest makeRequest() const;
    [[nodiscard]] MergeJobRequest makeMergeRequest() const;
    [[nodiscard]] JobEvents makeEvents();

    UiEngine& engine_;
    HardwareCapabilitySource* hardware_ = nullptr;
    bool userEnabledGpu_ = true;
    bool userEnabledHardwareDecoding_ = true;
    std::vector<QueueItem> queue_;
    int nextId_ = 1;
    int selectedId_ = 0;
    std::optional<ProbedSource> probed_;
    UserConstraint constraint_;
    NamingPolicy naming_;
    VerificationPolicy verification_;
    PlannerSettings plannerSettings_;
    std::string outputDirectory_;
    int ffmpegThreads_ = 0;
    bool lowPriority_ = false;
    std::optional<SplitPlan> preview_;
    bool previewStale_ = false;
    SessionSnapshot snapshot_;
    SystemMonitor monitor_;
    double jobStartSeconds_ = 0.0;

    std::mutex eventMutex_;
    std::deque<PendingEvent> events_;
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> pauseRequested_{false};
    std::thread worker_;
    std::atomic<bool> workerFinished_{true};
    std::vector<SegmentOutcome> workerParts_;
    std::string workerJobId_;
    bool workerVerified_ = false;
    bool workerCanceled_ = false;
    std::mutex workerMutex_;

    /// The merge session and its worker.  Separate from the split's because the two
    /// cannot overlap: a user who starts a merge while a split is running has asked
    /// for two jobs writing to the same output folder, and neither would finish
    /// cleanly.  `mergeBusy_` is what enforces that on both sides.
    MergeSession merge_;
    std::mutex mergeMutex_;
    std::thread mergeWorker_;
    std::atomic<bool> mergeWorkerFinished_{true};
    std::atomic<bool> mergeCancelRequested_{false};
    std::atomic<bool> mergeBusy_{false};

    ResourceSample lastSample_;
    std::string databasePath_;
    std::vector<JobRecord> history_;
    std::string historyProblem_;
    bool historyLoaded_ = false;

    bool helpVisible_ = false;
    const HelpTopic* helpTopic_ = nullptr;
    std::string helpQuery_;
    std::vector<const HelpTopic*> helpResults_;
};

} // namespace monosect::core
