#include "ui/UiController.hpp"

#include "core/Bytes.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace monosect::core {

UiEngine::~UiEngine() = default;

HardwareCapabilitySource::~HardwareCapabilitySource() = default;

EngineUiEngine::EngineUiEngine(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath)
    : prober_(prober), runner_(runner), ffmpegPath_(std::move(ffmpegPath)) {}

Result<ProbedSource> EngineUiEngine::probe(const std::string& path) {
    auto snapshot = prober_.probeAll(path);
    if (!snapshot.has_value()) {
        return snapshot.error();
    }
    ProbedSource source;
    source.model = std::move(snapshot.value().container);
    source.index = std::move(snapshot.value().packets);
    return source;
}

Result<SplitPlan> EngineUiEngine::plan(const JobRequest& request, const JobEvents& events) {
    JobRunner runner(prober_, runner_, ffmpegPath_);
    return runner.planOnly(request, events);
}

Result<JobOutcome> EngineUiEngine::run(const JobRequest& request, const JobEvents& events) {
    JobRunner runner(prober_, runner_, ffmpegPath_);
    return runner.run(request, events);
}

Result<MergePlan> EngineUiEngine::planMerge(const MergeJobRequest& request, const JobEvents& events) {
    // The runner is constructed per call rather than held: it owns no state that
    // outlives a job, and the split path already does the same, so there is one
    // answer to "what does a job need" rather than two.
    MergeRunner runner(prober_, runner_, ffmpegPath_);
    return runner.planOnly(request, events);
}

Result<MergeOutcome> EngineUiEngine::runMerge(const MergeJobRequest& request, const JobEvents& events) {
    MergeRunner runner(prober_, runner_, ffmpegPath_);
    return runner.run(request, events);
}

std::string EngineUiEngine::ffmpegVersion() {
    // Recorded in the job history.  A missing version is empty, never invented.
    return {};
}

std::string EngineUiEngine::ffprobeVersion() {
    auto version = prober_.version();
    return version.has_value() ? version.value() : std::string{};
}

const char* sessionPhaseName(SessionPhase phase) noexcept {
    switch (phase) {
        case SessionPhase::Empty: return "no source";
        case SessionPhase::Probing: return "reading the source";
        case SessionPhase::Ready: return "ready to plan";
        case SessionPhase::Planning: return "planning";
        case SessionPhase::Planned: return "plan ready";
        case SessionPhase::Running: return "running";
        case SessionPhase::Pausing: return "pausing";
        case SessionPhase::Paused: return "paused";
        case SessionPhase::Cancelling: return "cancelling";
        case SessionPhase::Completed: return "finished";
        case SessionPhase::Failed: return "failed";
    }
    return "unknown";
}

namespace {

std::string sourceHeadline(const QueueItem& item) {
    std::string text = formatBytesShort(item.sizeBytes);
    if (item.duration.isPositive()) {
        text.append(", ").append(formatDuration(item.duration));
    }
    text.append(", ").append(std::to_string(item.streamCount)).append(" stream(s)");
    if (!item.summary.empty()) {
        text.append(" - ").append(item.summary);
    }
    return text;
}

double steadyNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

UiController::UiController(UiEngine& engine, HardwareCapabilitySource* hardware)
    : engine_(engine), hardware_(hardware) {
    refreshHistory();
}

// -------------------------------------------------------------------------------------
// Queue

int UiController::addSource(const std::string& path) {
    QueueItem item;
    item.id = nextId_++;
    item.path = path;
    std::error_code error;
    const std::filesystem::path asPath(path);
    item.displayName = asPath.filename().string();
    if (item.displayName.empty()) {
        item.displayName = asPath.string();
    }
    const std::uintmax_t size = std::filesystem::file_size(asPath, error);
    if (!error) {
        item.sizeBytes = static_cast<ByteCount>(size);
    }
    item.status = "not read yet";
    queue_.push_back(std::move(item));
    select(queue_.back().id);
    return queue_.back().id;
}

void UiController::removeSource(int id) {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [id](const QueueItem& item) { return item.id == id; }),
                 queue_.end());
    if (selectedId_ == id) {
        selectedId_ = queue_.empty() ? 0 : queue_.front().id;
        probed_.reset();
        invalidatePreview();
        snapshot_.phase = queue_.empty() ? SessionPhase::Empty : SessionPhase::Empty;
        snapshot_.statusHeadline = queue_.empty() ? "No source selected" : "Select a source to read it";
        snapshot_.statusDetail.clear();
    }
}

void UiController::clearSources() {
    queue_.clear();
    selectedId_ = 0;
    probed_.reset();
    invalidatePreview();
    snapshot_ = SessionSnapshot{};
}

void UiController::select(int id) {
    const auto found = std::find_if(queue_.begin(), queue_.end(),
                                    [id](const QueueItem& item) { return item.id == id; });
    if (found == queue_.end()) {
        return;
    }
    selectedId_ = id;
    probed_.reset();
    invalidatePreview();
    if (found->probed) {
        snapshot_.phase = SessionPhase::Ready;
        snapshot_.statusHeadline = "Source ready";
        snapshot_.statusDetail = sourceHeadline(*found);
        snapshot_.tone = StatusTone::Neutral;
    } else {
        snapshot_.phase = SessionPhase::Empty;
        snapshot_.statusHeadline = "Not read yet - press Read";
        snapshot_.statusDetail.clear();
        snapshot_.tone = StatusTone::Neutral;
    }
    snapshot_.parts.clear();
    snapshot_.plan.reset();
}

const QueueItem* UiController::selected() const {
    const auto found = std::find_if(queue_.begin(), queue_.end(),
                                    [this](const QueueItem& item) { return item.id == selectedId_; });
    return found == queue_.end() ? nullptr : &*found;
}

// -------------------------------------------------------------------------------------
// Probing

void UiController::probeSelected() {
    const QueueItem* item = selected();
    if (item == nullptr) {
        snapshot_.phase = SessionPhase::Empty;
        snapshot_.statusHeadline = "Add a source first";
        snapshot_.tone = StatusTone::Warning;
        return;
    }
    const std::string path = item->path;
    snapshot_.phase = SessionPhase::Probing;
    snapshot_.statusHeadline = "Reading " + item->displayName;
    snapshot_.statusDetail = "Probing the container and indexing every packet.";
    snapshot_.tone = StatusTone::Working;
    pushLog("probing " + path);

    auto probed = engine_.probe(path);
    auto& mutableItem = *std::find_if(queue_.begin(), queue_.end(),
                                      [this](const QueueItem& entry) { return entry.id == selectedId_; });
    if (!probed.has_value()) {
        mutableItem.probed = false;
        mutableItem.problem = probed.error().toString();
        probed_.reset();
        invalidatePreview();
        snapshot_.phase = SessionPhase::Failed;
        snapshot_.statusHeadline = "The source could not be read";
        snapshot_.statusDetail = probed.error().message;
        if (!probed.error().detail.empty()) {
            snapshot_.statusDetail.append(" ").append(probed.error().detail);
        }
        pushLog("probe failed: " + mutableItem.problem);
        return;
    }
    probed_ = std::move(probed.value());
    mutableItem.probed = true;
    mutableItem.problem.clear();
    mutableItem.sizeBytes = probed_->model.fileSize;
    const DurationTruth truth = probed_->model.durationTruth();
    mutableItem.duration = truth.seconds.value_or(Rational{0, 1});
    mutableItem.streamCount = static_cast<int>(probed_->model.streams.size());
    mutableItem.summary = probed_->model.videoStream() != nullptr
                              ? probed_->model.videoStream()->parameterSummary()
                              : std::string("no video stream");
    mutableItem.status = "read";
    snapshot_.phase = SessionPhase::Ready;
    snapshot_.statusHeadline = "Source ready";
    snapshot_.statusDetail = sourceHeadline(mutableItem);
    snapshot_.tone = StatusTone::Neutral;
    pushLog("probe: " + snapshot_.statusDetail);
}

// -------------------------------------------------------------------------------------
// Constraint

void UiController::invalidatePreview() {
    if (preview_.has_value()) {
        previewStale_ = true;
    }
    snapshot_.plan = preview_;
    snapshot_.partsTotal = preview_.has_value() ? static_cast<int>(preview_->segments.size()) : 0;
}

void UiController::setSizeLimitBytes(ByteCount bytes) {
    if (bytes > 0) {
        SizeLimit limit;
        limit.maxBytes = bytes;
        constraint_.size = limit;
    } else {
        constraint_.size.reset();
    }
    invalidatePreview();
}

void UiController::clearSizeLimit() {
    constraint_.size.reset();
    invalidatePreview();
}

void UiController::setDurationLimit(Rational seconds) {
    if (seconds.isPositive()) {
        constraint_.duration = DurationLimit{seconds};
    } else {
        constraint_.duration.reset();
    }
    invalidatePreview();
}

void UiController::clearDurationLimit() {
    constraint_.duration.reset();
    invalidatePreview();
}

void UiController::setPreservation(PreservationMode mode) {
    constraint_.preservation = mode;
    invalidatePreview();
}

void UiController::setPrecision(PrecisionPolicy policy) {
    constraint_.precision = policy;
    invalidatePreview();
}

void UiController::setNamingPattern(const std::string& pattern) {
    naming_.pattern = pattern;
    invalidatePreview();
}

ConstraintReport UiController::validate() const {
    ConstraintReport report;
    if (probed_.has_value()) {
        report = validateConstraint(constraint_, probed_->model);
    } else {
        report.valid = false;
        ConstraintIssue issue;
        issue.severity = ConstraintIssue::Severity::Error;
        issue.message = "No source has been read yet.";
        issue.remedy = "Add a source and press Read.";
        report.issues.push_back(issue);
    }
    // The naming pattern is checked here too, because an invalid pattern would
    // otherwise fail after the first part had already been written.
    NamingContext context;
    if (const QueueItem* item = selected(); item != nullptr) {
        context.sourceStem = std::filesystem::path(item->path).stem().string();
    }
    context.extension = "mkv";
    context.index = 1;
    context.total = preview_.has_value() ? static_cast<int>(preview_->segments.size()) : 1;
    context.startSeconds = Rational{0, 1};
    context.endSeconds = Rational{1, 1};
    context.duration = Rational{1, 1};
    auto name = renderOutputName(naming_, context);
    if (!name.has_value()) {
        ConstraintIssue issue;
        issue.severity = ConstraintIssue::Severity::Error;
        issue.message = "The output name pattern is invalid: " + name.error().message;
        issue.remedy = "Use the placeholders {name} {index} {total} {ext}, or reset the pattern.";
        report.issues.push_back(issue);
        report.valid = false;
    }
    for (const ConstraintIssue& issue : report.issues) {
        if (issue.severity == ConstraintIssue::Severity::Error) {
            report.valid = false;
        }
    }
    return report;
}

// -------------------------------------------------------------------------------------
// Planning

JobRequest UiController::makeRequest() const {
    JobRequest request;
    if (const QueueItem* item = selected(); item != nullptr) {
        request.sourcePath = item->path;
    }
    request.outputDirectory = outputDirectory_;
    request.constraint = constraint_;
    request.naming = naming_;
    request.verification = verification_;
    request.plannerSettings = plannerSettings_;
    request.ffmpegThreads = ffmpegThreads_;
    request.lowPriority = lowPriority_;
    request.databasePath = databasePath_;
    request.recordHistory = true;
    request.jobTag = "ui";
    request.ffmpegVersion = engine_.ffmpegVersion();
    request.ffprobeVersion = engine_.ffprobeVersion();
    return request;
}

void UiController::previewPlan(const std::string& outputDirectory) {
    outputDirectory_ = outputDirectory;
    if (jobRunning()) {
        return;
    }
    const ConstraintReport report = validate();
    if (!report.valid) {
        preview_.reset();
        previewStale_ = false;
        snapshot_.phase = SessionPhase::Failed;
        snapshot_.tone = StatusTone::Bad;
        snapshot_.statusHeadline = "This request cannot be planned";
        snapshot_.statusDetail.clear();
        for (const ConstraintIssue& issue : report.issues) {
            if (!snapshot_.statusDetail.empty()) {
                snapshot_.statusDetail.append(" ");
            }
            snapshot_.statusDetail.append(issue.message);
            if (!issue.remedy.empty()) {
                snapshot_.statusDetail.append(" ").append(issue.remedy);
            }
        }
        return;
    }
    snapshot_.phase = SessionPhase::Planning;
    snapshot_.statusHeadline = "Planning";
    snapshot_.statusDetail = "Finding boundaries that respect the limits.";
    snapshot_.tone = StatusTone::Working;

    JobEvents events = makeEvents();
    auto planned = engine_.plan(makeRequest(), events);
    if (!planned.has_value()) {
        preview_.reset();
        snapshot_.phase = SessionPhase::Failed;
        snapshot_.tone = StatusTone::Bad;
        snapshot_.statusHeadline = "The plan was refused";
        snapshot_.statusDetail = planned.error().message;
        if (!planned.error().detail.empty()) {
            snapshot_.statusDetail.append(" ").append(planned.error().detail);
        }
        pushLog("planning failed: " + planned.error().toString());
        return;
    }
    preview_ = planned.value();
    previewStale_ = false;
    snapshot_.plan = preview_;
    snapshot_.partsTotal = static_cast<int>(preview_->segments.size());
    snapshot_.parts.clear();
    snapshot_.jobState = JobState::Planned;
    if (!preview_->planned) {
        snapshot_.phase = SessionPhase::Failed;
        snapshot_.tone = StatusTone::Bad;
        snapshot_.statusHeadline = "The plan is not usable";
        snapshot_.statusDetail = preview_->failureExplanation;
        return;
    }
    snapshot_.phase = SessionPhase::Planned;
    snapshot_.tone = StatusTone::Neutral;
    snapshot_.statusHeadline = std::to_string(preview_->segments.size()) + " part(s) planned";
    snapshot_.statusDetail = UiModel::planSummary(*preview_);
    for (const SegmentPlan& segment : preview_->segments) {
        SegmentOutcome outcome;
        outcome.index = segment.index;
        outcome.startSeconds = segment.startSeconds;
        outcome.endSeconds = segment.endSeconds;
        outcome.method = segment.method;
        outcome.fileSizeBytes = segment.predictedFileBytes;
        outcome.duration = subtractRational(segment.endSeconds, segment.startSeconds).value_or(Rational{0, 1});
        outcome.verificationSummary = "planned only";
        snapshot_.parts.push_back(outcome);
    }
}

void UiController::discardPreview() {
    preview_.reset();
    previewStale_ = false;
    snapshot_.plan.reset();
    snapshot_.parts.clear();
    snapshot_.partsTotal = 0;
    if (snapshot_.phase == SessionPhase::Planned) {
        snapshot_.phase = probed_.has_value() ? SessionPhase::Ready : SessionPhase::Empty;
        snapshot_.statusHeadline = "Preview discarded";
        snapshot_.statusDetail.clear();
    }
}

// -------------------------------------------------------------------------------------
// Running

JobEvents UiController::makeEvents() {
    JobEvents events;
    events.onState = [this](JobState state, const std::string& message) {
        std::lock_guard<std::mutex> lock(eventMutex_);
        PendingEvent event;
        event.kind = PendingEvent::Kind::State;
        event.state = state;
        event.text = message;
        events_.push_back(std::move(event));
    };
    events.onLog = [this](const std::string& line) {
        std::lock_guard<std::mutex> lock(eventMutex_);
        PendingEvent event;
        event.kind = PendingEvent::Kind::Log;
        event.text = line;
        events_.push_back(std::move(event));
    };
    events.onPlanReady = [this](const SplitPlan& plan) {
        std::lock_guard<std::mutex> lock(eventMutex_);
        PendingEvent event;
        event.kind = PendingEvent::Kind::Plan;
        event.plan = plan;
        events_.push_back(std::move(event));
    };
    events.onProgress = [this](int partIndex, int partCount, double fraction, const std::string& stage) {
        std::lock_guard<std::mutex> lock(eventMutex_);
        PendingEvent event;
        event.kind = PendingEvent::Kind::Progress;
        event.partIndex = partIndex;
        event.partCount = partCount;
        event.fraction = fraction;
        event.stage = stage;
        events_.push_back(std::move(event));
    };
    events.shouldCancel = [this]() { return cancelRequested_.load(); };
    events.shouldPause = [this]() { return pauseRequested_.load(); };
    return events;
}

void UiController::start(const std::string& outputDirectory) {
    outputDirectory_ = outputDirectory;
    if (jobRunning()) {
        snapshot_.statusDetail = "A job is already running.";
        return;
    }
    if (outputDirectory_.empty()) {
        snapshot_.phase = SessionPhase::Failed;
        snapshot_.tone = StatusTone::Warning;
        snapshot_.statusHeadline = "No output folder";
        snapshot_.statusDetail = "Choose the folder the parts are written to.";
        return;
    }
    const ConstraintReport report = validate();
    if (!report.valid) {
        snapshot_.phase = SessionPhase::Failed;
        snapshot_.tone = StatusTone::Bad;
        snapshot_.statusHeadline = "This request cannot be started";
        snapshot_.statusDetail = report.issues.empty() ? std::string("The request is not valid.")
                                                       : report.issues.front().message;
        return;
    }

    reapFinishedWorker();
    cancelRequested_ = false;
    pauseRequested_ = false;
    workerVerified_ = false;
    workerWarnings_.clear();
    workerCanceled_ = false;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        workerParts_.clear();
        workerJobId_.clear();
    }
    snapshot_.parts.clear();
    snapshot_.warnings.clear();
    snapshot_.verified = false;
    snapshot_.canceled = false;
    snapshot_.partsDone = 0;
    snapshot_.bytesWritten = 0;
    snapshot_.fraction = 0.0;
    snapshot_.elapsedSeconds = 0.0;
    snapshot_.phase = SessionPhase::Running;
    snapshot_.tone = StatusTone::Working;
    snapshot_.statusHeadline = "Starting";
    snapshot_.statusDetail = UiModel::constraintHeadline(constraint_);
    jobStartSeconds_ = steadyNow();

    const JobRequest request = makeRequest();
    workerFinished_ = false;
    worker_ = std::thread([this, request]() {
        const JobEvents events = makeEvents();
        Result<JobOutcome> result = engine_.run(request, events);
        {
            std::lock_guard<std::mutex> lock(workerMutex_);
            if (result.has_value()) {
                workerParts_ = result.value().parts;
                workerJobId_ = result.value().jobId;
                workerVerified_ = result.value().succeeded && !result.value().parts.empty() &&
                                  std::all_of(result.value().parts.begin(), result.value().parts.end(),
                                              [](const SegmentOutcome& part) { return part.verified; });
                workerCanceled_ = result.value().canceled;
                workerWarnings_ = result.value().warnings;
            }
        }
        std::lock_guard<std::mutex> lock(eventMutex_);
        PendingEvent event;
        event.kind = PendingEvent::Kind::State;
        if (result.has_value()) {
            event.state = result.value().finalState;
            event.text = result.value().message;
        } else {
            event.state = JobState::Failed;
            event.text = result.error().toString();
        }
        events_.push_back(std::move(event));
        workerFinished_ = true;
    });
}

void UiController::requestCancel() {
    if (!jobRunning()) {
        return;
    }
    cancelRequested_ = true;
    pauseRequested_ = false;
    snapshot_.phase = SessionPhase::Cancelling;
    snapshot_.tone = StatusTone::Warning;
    // Honest wording: the part being written is always finished, so cancelling
    // does not stop the file in the middle.
    snapshot_.statusHeadline = "Cancelling";
    snapshot_.statusDetail = "The part being written is finished first; already published parts stay on disk.";
}

void UiController::requestPause() {
    if (!jobRunning() || pauseRequested_.load()) {
        return;
    }
    pauseRequested_ = true;
    snapshot_.phase = SessionPhase::Pausing;
    snapshot_.tone = StatusTone::Warning;
    snapshot_.statusHeadline = "Pausing";
    snapshot_.statusDetail = "The pause takes effect at the next part boundary; the current part is finished.";
}

void UiController::requestResume() {
    if (!jobRunning() || !pauseRequested_.load()) {
        return;
    }
    pauseRequested_ = false;
    snapshot_.phase = SessionPhase::Running;
    snapshot_.tone = StatusTone::Working;
    snapshot_.statusHeadline = "Resuming";
    snapshot_.statusDetail = "The job continues with the next part.";
}

void UiController::poll() {
    reapFinishedWorker();
    reapFinishedMergeWorker();
    std::deque<PendingEvent> drained;
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        drained.swap(events_);
    }
    for (PendingEvent& event : drained) {
        switch (event.kind) {
            case PendingEvent::Kind::Log:
                pushLog(event.text);
                break;
            case PendingEvent::Kind::Plan:
                preview_ = event.plan;
                previewStale_ = false;
                snapshot_.plan = preview_;
                snapshot_.partsTotal = static_cast<int>(event.plan.segments.size());
                break;
            case PendingEvent::Kind::Progress:
                snapshot_.partsDone = event.partIndex;
                snapshot_.partsTotal = event.partCount;
                snapshot_.fraction = event.fraction;
                snapshot_.stage = event.stage;
                break;
            case PendingEvent::Kind::State:
                snapshot_.jobState = event.state;
                if (!event.text.empty()) {
                    snapshot_.statusDetail = event.text;
                }
                switch (event.state) {
                    case JobState::Planning:
                        snapshot_.phase = SessionPhase::Planning;
                        snapshot_.statusHeadline = "Planning";
                        snapshot_.tone = StatusTone::Working;
                        break;
                    case JobState::Planned:
                        snapshot_.phase = SessionPhase::Running;
                        snapshot_.statusHeadline = "Running";
                        snapshot_.tone = StatusTone::Working;
                        break;
                    case JobState::Executing:
                        snapshot_.phase = SessionPhase::Running;
                        snapshot_.statusHeadline = "Writing parts";
                        snapshot_.tone = StatusTone::Working;
                        break;
                    case JobState::Verifying:
                        snapshot_.phase = SessionPhase::Running;
                        snapshot_.statusHeadline = "Verifying";
                        snapshot_.tone = StatusTone::Working;
                        break;
                    case JobState::Publishing:
                        snapshot_.phase = SessionPhase::Running;
                        snapshot_.statusHeadline = "Publishing";
                        snapshot_.tone = StatusTone::Working;
                        break;
                    case JobState::Paused:
                        snapshot_.phase = SessionPhase::Paused;
                        snapshot_.statusHeadline = "Paused";
                        snapshot_.tone = StatusTone::Warning;
                        break;
                    case JobState::Completed:
                        snapshot_.phase = SessionPhase::Completed;
                        snapshot_.statusHeadline = "Finished";
                        snapshot_.tone = StatusTone::Good;
                        break;
                    case JobState::Cancelled:
                        snapshot_.phase = SessionPhase::Completed;
                        snapshot_.canceled = true;
                        snapshot_.statusHeadline = "Cancelled";
                        snapshot_.tone = StatusTone::Warning;
                        break;
                    case JobState::Failed:
                        snapshot_.phase = SessionPhase::Failed;
                        snapshot_.tone = StatusTone::Bad;
                        snapshot_.statusHeadline = "Failed";
                        break;
                    case JobState::Idle:
                        break;
                }
                break;
        }
    }

    // Results arrive from the worker; copy them once the worker is done so the
    // tables cannot show a half-written list.
    if (workerFinished_.load() || worker_.joinable()) {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (!workerParts_.empty()) {
            snapshot_.parts = workerParts_;
            snapshot_.partsDone = static_cast<int>(
                std::count_if(workerParts_.begin(), workerParts_.end(),
                              [](const SegmentOutcome& part) { return part.wrote; }));
            ByteCount written = 0;
            for (const SegmentOutcome& part : workerParts_) {
                if (part.committed) {
                    written += part.fileSizeBytes;
                }
            }
            snapshot_.bytesWritten = written;
        }
        if (!workerJobId_.empty()) {
            snapshot_.jobId = workerJobId_;
        }
        snapshot_.verified = workerVerified_;
        snapshot_.warnings = workerWarnings_;
        if (workerCanceled_) {
            snapshot_.canceled = true;
        }
    }
    snapshot_.elapsedSeconds = jobStartSeconds_ > 0.0 ? steadyNow() - jobStartSeconds_ : 0.0;
    if (snapshot_.phase == SessionPhase::Planned && previewIsStale()) {
        snapshot_.tone = StatusTone::Warning;
        snapshot_.statusHeadline = "The plan is out of date";
        snapshot_.statusDetail = "The limits changed after this plan was made. Plan again before starting.";
    }
}

void UiController::reapFinishedWorker() {
    if (worker_.joinable() && workerFinished_.load()) {
        worker_.join();
    }
}

void UiController::waitForJob() {
    if (worker_.joinable()) {
        worker_.join();
        workerFinished_ = true;
    }
    poll();
}

void UiController::pushLog(const std::string& line) {
    snapshot_.log.push_back(line);
    // The window keeps a bounded log; the file on disk keeps everything.
    constexpr std::size_t kMaxLogLines = 2000;
    if (snapshot_.log.size() > kMaxLogLines) {
        snapshot_.log.erase(snapshot_.log.begin(), snapshot_.log.begin() + 500);
    }
}

// -------------------------------------------------------------------------------------
// Resources and history

const ResourceSample& UiController::sampleResources() {
    const SessionSnapshot snapshot = snapshot_;
    ResourceSample sample = monitor_.sample(static_cast<std::int64_t>(snapshot.bytesWritten),
                                            snapshot.elapsedSeconds, 0.0);
    lastSample_ = std::move(sample);
    return lastSample_;
}

ResourcePolicyInput UiController::resourceInput() const {
    ResourcePolicyInput input;
    input.topology.logicalCores = monitor_.logicalCores();
    input.topology.physicalCores = monitor_.physicalCores();
    // The GPU capability comes from the probe, not from an assumption.  The comment
    // this replaces said the caller would fill it in - and no caller did, so
    // ResourcePolicy always saw a GPU that was neither present nor usable and the
    // resource panel could not tell a machine with no card from one MonoSect had
    // not asked yet.  toGpuCapability distinguishes those three cases itself, which
    // is exactly why it exists.
    if (hardware_ != nullptr) {
        input.gpu = hardware_->capabilities().toGpuCapability(userEnabledGpu_, jobWillEncode());
    }
    input.userEnabledGpu = userEnabledGpu_;
    input.userEnabledHardwareDecoding = userEnabledHardwareDecoding_;
    input.jobWillEncode = jobWillEncode();
    return input;
}

bool UiController::jobWillEncode() const noexcept {
    // A stream copy needs no encoder at all, so GPU settings only come into play
    // when the plan has to re-encode a boundary.
    if (!preview_.has_value()) {
        return false;
    }
    for (const SegmentPlan& segment : preview_->segments) {
        if (!segment.reencodedStreamIndexes.empty()) {
            return true;
        }
    }
    return false;
}

bool UiController::hardwareProbed() const {
    return hardware_ != nullptr && hardware_->capabilities().probed;
}

bool UiController::hardwareProbeRunning() const {
    return hardware_ != nullptr && hardware_->probeRunning();
}

std::string UiController::hardwareSummary() const {
    if (hardware_ == nullptr) {
        return "The hardware encoder probe is not available in this build.";
    }
    if (hardwareProbeRunning()) {
        return "Testing whether this machine's hardware encoders work. "
               "A listed encoder is not a working one, so each one is asked to encode a frame.";
    }
    return hardware_->capabilities().describe();
}

std::vector<EncoderProbe> UiController::hardwareEncoders() const {
    return hardware_ != nullptr ? hardware_->capabilities().encoders : std::vector<EncoderProbe>();
}

void UiController::setDatabasePath(const std::string& path) {
    if (databasePath_ != path) {
        databasePath_ = path;
        historyLoaded_ = false;
        refreshHistory();
    }
}

void UiController::refreshHistory() {
    if (!databasePath_.empty()) {
        // A path that exists but is not a file (a folder, a device) is a
        // problem to report, not an empty history to hide behind.
        std::error_code error;
        const std::filesystem::path path(databasePath_);
        if (std::filesystem::exists(path, error) && !std::filesystem::is_regular_file(path, error)) {
            history_.clear();
            historyProblem_ = "The job history path is not a file: " + databasePath_;
            return;
        }
    }
    JobStore store = databasePath_.empty() ? JobStore::defaultStore() : JobStore(databasePath_);
    auto records = store.loadRecent(200);
    if (!records.has_value()) {
        history_.clear();
        historyProblem_ = "The job history could not be read: " + records.error().message;
        return;
    }
    historyProblem_.clear();
    history_ = std::move(records.value());
    historyLoaded_ = true;
}

// -------------------------------------------------------------------------------------
// Help

void UiController::showHelp(HelpTopicId id) {
    helpTopic_ = HelpContent::find(id);
    helpVisible_ = helpTopic_ != nullptr;
    helpQuery_.clear();
    helpResults_.clear();
}

void UiController::closeHelp() {
    helpVisible_ = false;
    helpTopic_ = nullptr;
}

void UiController::setHelpQuery(const std::string& query) {
    helpQuery_ = query;
    helpResults_ = HelpContent::search(query);
    if (!helpResults_.empty()) {
        helpTopic_ = helpResults_.front();
    }
}

// -------------------------------------------------------------------------------------
// What the panels read

JobStatusLine UiController::statusLine() const {
    if (snapshot_.phase == SessionPhase::Empty && snapshot_.statusHeadline.empty()) {
        JobStatusLine line;
        line.headline = "Add a source to begin";
        line.detail = "MonoSect splits a file into parts that never exceed the limits you set.";
        return line;
    }
    JobStatusLine line;
    line.headline = snapshot_.statusHeadline;
    line.detail = snapshot_.statusDetail;
    line.tone = snapshot_.tone;
    if (snapshot_.phase == SessionPhase::Completed && !snapshot_.canceled) {
        if (snapshot_.verified) {
            line.headline = "Finished and verified";
            line.tone = StatusTone::Good;
        } else {
            line.headline = "Finished (not verified)";
            line.tone = StatusTone::Warning;
        }
    }
    return line;
}

ProgressView UiController::progressView() const {
    std::optional<double> remaining;
    if (snapshot_.phase == SessionPhase::Running && snapshot_.fraction > 0.01 && snapshot_.elapsedSeconds > 0.5) {
        remaining = snapshot_.elapsedSeconds * (1.0 - snapshot_.fraction) / snapshot_.fraction;
    }
    return UiModel::progress(snapshot_.partsDone, snapshot_.partsTotal, static_cast<double>(snapshot_.bytesWritten),
                             remaining);
}

std::vector<UiModel::PartLine> UiController::partLines() const {
    if (snapshot_.plan.has_value()) {
        return UiModel::partLines(snapshot_.plan.value());
    }
    return {};
}

JobStatusLine UiController::verificationLine() const {
    if (snapshot_.parts.empty()) {
        JobStatusLine line;
        line.headline = "Nothing has been written yet";
        line.detail = "Each part is verified after it is written; results appear here.";
        return line;
    }
    VerificationResult result;
    result.passed = snapshot_.verified && !snapshot_.canceled && std::all_of(snapshot_.parts.begin(), snapshot_.parts.end(),
                                                       [](const SegmentOutcome& part) { return part.verified; });
    std::size_t verified = 0;
    for (const SegmentOutcome& part : snapshot_.parts) {
        if (part.verified) {
            ++verified;
        }
        for (const std::string& finding : part.findings) {
            VerificationFinding entry;
            entry.severity = FindingSeverity::Error;
            entry.message = finding;
            result.findings.push_back(entry);
        }
    }
    result.summary = std::to_string(verified) + " of " + std::to_string(snapshot_.parts.size()) +
                     " written part(s) verified";
    return UiModel::verificationLine(result);
}



void UiController::showMerge() {
    merge_.active = true;
}

void UiController::hideMerge() {
    // A running merge is not abandoned by closing the panel: the worker would keep
    // writing with nothing watching it, and a file the user cannot see being made is
    // worse than a panel that will not close.  A finished one closes freely.
    if (mergeRunning()) {
        merge_.headline = "A merge is still running. Cancel it before closing the panel.";
        return;
    }
    merge_.active = false;
}

void UiController::addMergeInput(const std::string& path) {
    if (path.empty()) {
        return;
    }
    merge_.inputs.push_back(path);
    // The plan was made for the inputs as they were, so it is now stale.  Saying so
    // is better than leaving a summary that describes a different set of files.
    merge_.planned = false;
    merge_.planSummary.clear();
}

void UiController::removeMergeInput(std::size_t index) {
    if (index >= merge_.inputs.size()) {
        return;
    }
    merge_.inputs.erase(merge_.inputs.begin() + static_cast<std::ptrdiff_t>(index));
    merge_.planned = false;
    merge_.planSummary.clear();
}

void UiController::clearMergeInputs() {
    merge_.inputs.clear();
    merge_.planned = false;
    merge_.planSummary.clear();
    merge_.planProblem.clear();
}

void UiController::moveMergeInput(std::size_t index, int delta) {
    const std::size_t count = merge_.inputs.size();
    if (index >= count) {
        return;
    }
    // Computed in signed arithmetic: `index + delta` with unsigned index and a
    // negative delta wraps, which would move an input from the top of the list to
    // the bottom instead of doing nothing.
    const long target = static_cast<long>(index) + delta;
    if (target < 0 || target >= static_cast<long>(count)) {
        return;
    }
    const auto to = static_cast<std::size_t>(target);
    std::swap(merge_.inputs[index], merge_.inputs[to]);
    // The order is the join order, so it is part of what the plan describes.
    merge_.planned = false;
    merge_.planSummary.clear();
}

void UiController::previewMerge(const std::string& outputDirectory, const std::string& outputName) {
    merge_.outputDirectory = outputDirectory;
    merge_.outputName = outputName;
    merge_.planned = false;
    merge_.planSummary.clear();
    merge_.planProblem.clear();

    if (merge_.inputs.size() < 2) {
        merge_.planProblem = "A merge needs at least two files.";
        return;
    }
    if (jobRunning() || mergeRunning()) {
        merge_.planProblem = "Another job is running. Wait for it to finish.";
        return;
    }
    if (outputDirectory.empty()) {
        merge_.planProblem = "Choose the folder the merged file is written to.";
        return;
    }

    MergeJobRequest request = makeMergeRequest();
    const JobEvents events = makeEvents();
    const Result<MergePlan> plan = engine_.planMerge(request, events);
    if (!plan.has_value()) {
        // The refusal is shown as the plan's own problem rather than thrown away:
        // "these two files have different codecs" is the answer the user needs.
        merge_.planProblem = plan.error().toString();
        return;
    }
    merge_.planned = true;
    merge_.planSummary = plan.value().describe();
    for (const std::string& warning : plan.value().warnings) {
        merge_.warnings.push_back(warning);
    }
}

MergeJobRequest UiController::makeMergeRequest() const {
    MergeJobRequest request;
    request.inputs = merge_.inputs;
    request.outputDirectory = merge_.outputDirectory;
    request.outputFileName = merge_.outputName;
    request.overwriteExisting = false;
    request.lowPriority = lowPriority_;
    request.threads = ffmpegThreads_;
    request.databasePath = databasePath_;
    request.recordHistory = true;
    request.jobTag = "merge";
#ifdef MONOSECT_VERSION_STR
    request.appVersion = MONOSECT_VERSION_STR;
#else
    request.appVersion = "0.0.0";
#endif
    request.ffmpegVersion = engine_.ffmpegVersion();
    request.ffprobeVersion = engine_.ffprobeVersion();
    return request;
}

void UiController::startMerge() {
    if (mergeRunning()) {
        return;
    }
    if (!merge_.planned) {
        merge_.message = "Preview the merge before starting it.";
        return;
    }
    // One job at a time, in either direction.  Two jobs writing into the same output
    // folder would collide on names and leave both job records claiming work the
    // other overwrote.
    if (jobRunning()) {
        merge_.message = "A split job is running. Wait for it to finish.";
        return;
    }

    mergeCancelRequested_ = false;
    mergeWorkerFinished_ = false;
    mergeBusy_ = true;
    merge_.running = true;
    merge_.succeeded = false;
    merge_.verified = false;
    merge_.findings.clear();
    merge_.warnings.clear();
    merge_.message.clear();
    merge_.headline = "Joining " + std::to_string(merge_.inputs.size()) + " files...";
    merge_.fileSizeBytes = 0;
    merge_.durationKnown = false;

    const MergeJobRequest request = makeMergeRequest();
    mergeWorker_ = std::thread([this, request, completed = merge_]() mutable {
        JobEvents events = makeEvents();
        events.shouldCancel = [this] { return mergeCancelRequested_.load(); };
        const Result<MergeOutcome> result = engine_.runMerge(request, events);
        {
            if (result.has_value()) {
                const MergeOutcome& outcome = result.value();
                completed.succeeded = outcome.succeeded && !outcome.canceled;
                completed.canceled = outcome.canceled;
                completed.message = outcome.message;
                completed.path = outcome.path;
                completed.fileSizeBytes = outcome.fileSizeBytes;
                completed.duration = outcome.duration;
                completed.durationKnown = outcome.durationMeasured;
                completed.verified = outcome.verified;
                completed.findings = outcome.findings;
                completed.warnings = outcome.warnings;
                completed.jobId = outcome.jobId;
            } else {
                completed.succeeded = false;
                completed.message = result.error().toString();
            }
        }
        completed.running = false;
        mergeResult_ = std::move(completed);
        mergeWorkerFinished_ = true;
    });
}

UiController::~UiController() {
    // Both workers own a std::thread, and destroying a joinable one calls
    // std::terminate.  Cancelling first is what makes the join return promptly; the
    // join is unconditional because a destructor has no safe alternative.
    requestMergeCancel();
    if (mergeWorker_.joinable()) {
        mergeWorker_.join();
    }
    requestCancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void UiController::waitForMerge() {
    if (mergeWorker_.joinable()) {
        mergeWorker_.join();
    }
    reapFinishedMergeWorker();
}

void UiController::reapFinishedMergeWorker() {
    if (!mergeWorkerFinished_.load()) {
        return;
    }
    if (mergeWorker_.joinable()) {
        mergeWorker_.join();
    }
    if (mergeResult_.has_value()) {
        merge_ = std::move(*mergeResult_);
        mergeResult_.reset();
        mergeBusy_ = false;
    }
}

void UiController::requestMergeCancel() {
    if (!mergeRunning()) {
        return;
    }
    mergeCancelRequested_ = true;
    merge_.headline = "Cancelling...";
}


} // namespace monosect::core
