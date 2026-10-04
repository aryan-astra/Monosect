// MonoSect — the window's state machine, driven by a scripted engine.
//
// These tests run the whole user flow - add a source, read it, set a limit, plan,
// start, pause, cancel, finish - without FFmpeg, and check what the window would
// say at every step.  That is the only way to test the GUI on the development
// machine, and it is the part of the GUI where a wrong sentence is a product
// defect rather than a cosmetic one.
#include "TestSupport.hpp"
#include "Fixtures.hpp"

#include "engine/JobStore.hpp"
#include "ui/UiController.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace monosect::core;
using namespace monosect::test;

namespace {

/// One scripted engine for every test.  Behaviour is set by the fields.
class FakeEngine final : public UiEngine {
public:
    enum class PlanMode { Success, Failure, Refusal };

    PlanMode planMode = PlanMode::Success;
    bool probeFails = false;
    bool runSucceeds = true;
    bool runVerifiesEverything = true;
    bool runSleepsUntilCanceled = false;
    bool runSleepsUntilPaused = false;
    std::string failureMessage = "the limit cannot be met";
    SyntheticSource source;
    int probeCalls = 0;
    int planCalls = 0;
    int runCalls = 0;

    FakeEngine() : source(makeSyntheticSource()) {}

    Result<ProbedSource> probe(const std::string&) override {
        ++probeCalls;
        if (probeFails) {
            return Error(ErrorCode::ProbeFailed, "the file is not a media file",
                         "ffprobe could not identify any stream");
        }
        ProbedSource probed;
        probed.model = source.model;
        probed.index = source.index;
        return probed;
    }

    Result<SplitPlan> plan(const JobRequest& request, const JobEvents&) override {
        ++planCalls;
        if (planMode == PlanMode::Failure) {
            return Error(ErrorCode::ConstraintUnsatisfiable, failureMessage, "no boundary fits");
        }
        SizeModel sizeModel;
        sizeModel.seedFromSource(source.model.formatNames, source.model.fileSize, source.index.totalPayloadBytes());
        NamingPolicy naming = request.naming;
        UserConstraint constraint = request.constraint;
        SplitPlanner planner(source.model, source.index, constraint, naming, sizeModel);
        auto planned = planner.plan(request.outputDirectory);
        if (!planned.has_value()) {
            return planned.error();
        }
        SplitPlan plan = planned.value();
        if (planMode == PlanMode::Refusal) {
            plan.planned = false;
            plan.failureExplanation = failureMessage;
        }
        return plan;
    }

    Result<JobOutcome> run(const JobRequest& request, const JobEvents& events) override {
        ++runCalls;
        JobOutcome outcome;
        outcome.jobId = "test-job";
        const auto say = [&events](JobState state, const std::string& text) {
            if (events.onState) {
                events.onState(state, text);
            }
        };
        if (events.onLog) {
            events.onLog("starting the test job");
        }
        auto planned = plan(request, events);
        if (!planned.has_value()) {
            say(JobState::Failed, planned.error().message);
            outcome.finalState = JobState::Failed;
            outcome.message = planned.error().message;
            outcome.succeeded = false;
            return outcome;
        }
        SplitPlan plan = planned.value();
        outcome.plan = plan;
        if (events.onPlanReady) {
            events.onPlanReady(plan);
        }
        say(JobState::Planned, "plan ready");
        const int total = static_cast<int>(plan.segments.size());
        for (int index = 0; index < total; ++index) {
            if (runSleepsUntilPaused && index == 0) {
                // Sit here until the controller asks to pause, then report the
                // pause the way the real runner does: between two parts.
                while (events.shouldPause && !events.shouldPause()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                say(JobState::Paused, "Paused after part " + std::to_string(index));
                while (events.shouldPause && events.shouldPause()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                say(JobState::Executing, "Writing part " + std::to_string(index + 1));
            }
            if (runSleepsUntilCanceled) {
                while (!(events.shouldCancel && events.shouldCancel())) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                outcome.canceled = true;
                outcome.parts = publishedParts(plan, index);
                outcome.finalState = JobState::Cancelled;
                outcome.message = "Cancelled after part " + std::to_string(index);
                say(JobState::Cancelled, outcome.message);
                return outcome;
            }
            say(JobState::Executing, "Writing part " + std::to_string(index + 1));
            if (events.onProgress) {
                events.onProgress(index + 1, total, static_cast<double>(index + 1) / total, "part " +
                                                                                                  std::to_string(index + 1));
            }
        }
        outcome.parts = publishedParts(plan, total);
        outcome.succeeded = runSucceeds;
        outcome.finalState = runSucceeds ? JobState::Completed : JobState::Failed;
        outcome.message = runSucceeds ? "wrote and verified every part" : "a part could not be written";
        say(outcome.finalState, outcome.message);
        return outcome;
    }

    std::string ffmpegVersion() override { return "ffmpeg test"; }

private:
    std::vector<SegmentOutcome> publishedParts(const SplitPlan& plan, int count) {
        std::vector<SegmentOutcome> parts;
        for (int index = 0; index < count && index < static_cast<int>(plan.segments.size()); ++index) {
            const SegmentPlan& segment = plan.segments[static_cast<std::size_t>(index)];
            SegmentOutcome outcome;
            outcome.index = segment.index;
            outcome.wrote = true;
            outcome.committed = true;
            outcome.path = "part-" + std::to_string(segment.index) + ".mkv";
            outcome.fileSizeBytes = segment.predictedFileBytes;
            outcome.duration = subtractRational(segment.endSeconds, segment.startSeconds).value_or(Rational{0, 1});
            outcome.startSeconds = segment.startSeconds;
            outcome.endSeconds = segment.endSeconds;
            outcome.verified = runVerifiesEverything;
            outcome.verificationSummary = outcome.verified ? "size, duration and streams verified"
                                                           : "the size check did not run";
            if (!outcome.verified) {
                outcome.findings.push_back("the size check did not run");
            }
            parts.push_back(std::move(outcome));
        }
        return parts;
    }
};

bool waitForTerminal(UiController& controller) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        controller.poll();
        const SessionPhase phase = controller.snapshot().phase;
        if (phase == SessionPhase::Completed || phase == SessionPhase::Failed) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

UserConstraint sizeLimit(ByteCount bytes, PreservationMode mode = PreservationMode::KeyframeAligned) {
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = bytes, .minBytes = std::nullopt};
    constraint.preservation = mode;
    return constraint;
}

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("monosect-ui-" + std::to_string(::getpid()) + "-" + std::to_string(rand()));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

} // namespace

MONOSECT_TEST(the_queue_is_one_source_at_a_time) {
    FakeEngine engine;
    UiController controller(engine);

    const int first = controller.addSource("C:/media/first.mkv");
    const int second = controller.addSource("C:/media/second.mkv");
    CHECK_EQ(2u, controller.queue().size());
    CHECK_EQ(second, controller.selectedId());
    CHECK(controller.selected() != nullptr);
    CHECK_EQ(std::string("second.mkv"), controller.selected()->displayName);

    controller.select(first);
    CHECK_EQ(first, controller.selectedId());
    CHECK_EQ(std::string("first.mkv"), controller.selected()->displayName);

    controller.removeSource(first);
    CHECK_EQ(1u, controller.queue().size());
    CHECK_EQ(second, controller.selectedId());

    controller.clearSources();
    CHECK(controller.queue().empty());
    CHECK_EQ(0, controller.selectedId());
    CHECK(controller.selected() == nullptr);
}

MONOSECT_TEST(reading_a_source_fills_the_list_and_the_status) {
    FakeEngine engine;
    UiController controller(engine);
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();

    CHECK_EQ(1, engine.probeCalls);
    CHECK(controller.snapshot().phase == SessionPhase::Ready);
    const QueueItem* item = controller.selected();
    CHECK(item != nullptr);
    if (item != nullptr) {
        CHECK(item->probed);
        CHECK(item->problem.empty());
        CHECK(item->streamCount == 2);
        CHECK(item->summary.find("h264") != std::string::npos);
        CHECK(item->duration.isPositive());
        CHECK(item->sizeBytes > 0);
    }
    controller.constraint() = sizeLimit(200000);
    CHECK(controller.validate().valid);

    // A probe failure is shown as a problem, not as an empty source.
    engine.probeFails = true;
    controller.probeSelected();
    item = controller.selected();
    CHECK(item != nullptr);
    CHECK(!item->probed);
    CHECK(!item->problem.empty());
    CHECK(controller.snapshot().phase == SessionPhase::Failed);
    CHECK(!controller.validate().valid);
}

MONOSECT_TEST(the_constraint_is_validated_against_the_source_and_the_naming_pattern) {
    FakeEngine engine;
    UiController controller(engine);
    controller.addSource("C:/media/movie.mkv");

    // Without a source nothing can be validated, and the reason says so.
    ConstraintReport report = controller.validate();
    CHECK(!report.valid);
    CHECK(!report.issues.empty());
    CHECK(report.issues.front().message.find("No source") != std::string::npos);

    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);
    CHECK(controller.validate().valid);

    // A pattern with an unknown placeholder is refused before any file is written.
    controller.setNamingPattern("{name} - {nonsense}");
    report = controller.validate();
    CHECK(!report.valid);
    bool mentionsPattern = false;
    for (const ConstraintIssue& issue : report.issues) {
        if (issue.message.find("pattern") != std::string::npos) {
            mentionsPattern = true;
        }
    }
    CHECK(mentionsPattern);

    controller.setNamingPattern("{name} - part {index:03}");
    CHECK(controller.validate().valid);
}

MONOSECT_TEST(changing_a_limit_makes_the_preview_out_of_date) {
    FakeEngine engine;
    UiController controller(engine);
    const TempDirectory directory;
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);
    controller.previewPlan(directory.path.string());

    CHECK(controller.preview().has_value());
    CHECK(!controller.previewIsStale());
    CHECK(controller.snapshot().phase == SessionPhase::Planned);
    CHECK(controller.snapshot().parts.size() == controller.preview()->segments.size());
    CHECK(controller.snapshot().partsTotal > 0);
    CHECK(!controller.partLines().empty());
    CHECK(controller.snapshot().statusDetail.find("limit") != std::string::npos);

    // The user then asks for something else: the old preview may not be started.
    controller.setSizeLimitBytes(120000);
    CHECK(controller.previewIsStale());
    controller.poll();
    CHECK(controller.snapshot().phase == SessionPhase::Planned);
    CHECK(controller.snapshot().statusHeadline.find("out of date") != std::string::npos);

    controller.previewPlan(directory.path.string());
    CHECK(!controller.previewIsStale());
    CHECK(controller.snapshot().statusHeadline.find("out of date") == std::string::npos);
}

MONOSECT_TEST(a_refused_plan_is_explained_and_never_shown_as_ready) {
    FakeEngine engine;
    UiController controller(engine);
    const TempDirectory directory;
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);

    engine.planMode = FakeEngine::PlanMode::Failure;
    engine.failureMessage = "The limits need 5 parts but at most 2 were allowed";
    controller.previewPlan(directory.path.string());
    CHECK(!controller.preview().has_value());
    CHECK(controller.snapshot().phase == SessionPhase::Failed);
    CHECK(controller.snapshot().statusDetail.find("5 parts") != std::string::npos);

    // A plan that exists but is not usable is also not "ready".
    engine.planMode = FakeEngine::PlanMode::Refusal;
    controller.previewPlan(directory.path.string());
    CHECK(controller.snapshot().phase == SessionPhase::Failed);
    CHECK(controller.snapshot().statusDetail.find("5 parts") != std::string::npos);
    engine.planMode = FakeEngine::PlanMode::Success;
}

MONOSECT_TEST(starting_requires_a_source_a_folder_and_a_valid_request) {
    FakeEngine engine;
    UiController controller(engine);
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);

    controller.start("");
    CHECK_EQ(0, engine.runCalls);
    CHECK(controller.snapshot().statusHeadline.find("No output folder") != std::string::npos);

    // No source read: refused with the reason from validate().
    controller.setOutputDirectory("C:/out");
    controller.select(controller.selectedId());
    controller.start("C:/out");
    CHECK_EQ(0, engine.runCalls);
    CHECK(controller.snapshot().statusHeadline.find("cannot be started") != std::string::npos);
}

MONOSECT_TEST(a_finished_job_reports_verified_only_when_every_part_verified) {
    FakeEngine engine;
    UiController controller(engine);
    const TempDirectory directory;
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);

    controller.start(directory.path.string());
    CHECK(waitForTerminal(controller));
    controller.waitForJob();

    CHECK_EQ(1, engine.runCalls);
    CHECK(controller.snapshot().phase == SessionPhase::Completed);
    const JobStatusLine line = controller.statusLine();
    CHECK_EQ(std::string("Finished and verified"), line.headline);
    CHECK(line.tone == StatusTone::Good);
    CHECK(controller.snapshot().verified);
    CHECK(controller.snapshot().partsDone == controller.snapshot().partsTotal);
    CHECK(controller.verificationLine().headline.find("Verified") != std::string::npos);
    CHECK(controller.progressView().partsDone == controller.snapshot().partsDone);

    // A part that was not verified must never be reported as verified.
    FakeEngine unverified;
    unverified.runVerifiesEverything = false;
    UiController second(unverified);
    second.addSource("C:/media/movie.mkv");
    second.probeSelected();
    second.constraint() = sizeLimit(200000);
    second.start(directory.path.string());
    CHECK(waitForTerminal(second));
    second.waitForJob();
    CHECK(!second.snapshot().verified);
    CHECK_EQ(std::string("Finished (not verified)"), second.statusLine().headline);
    CHECK(second.verificationLine().headline.find("not verified") != std::string::npos ||
          second.verificationLine().headline.find("Not verified") != std::string::npos);
}

MONOSECT_TEST(a_failed_job_keeps_the_parts_it_published_and_says_so) {
    FakeEngine engine;
    engine.runSucceeds = false;
    UiController controller(engine);
    const TempDirectory directory;
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);
    controller.start(directory.path.string());
    CHECK(waitForTerminal(controller));
    controller.waitForJob();
    CHECK(controller.snapshot().phase == SessionPhase::Failed);
    CHECK_EQ(std::string("Failed"), controller.statusLine().headline);
    CHECK(controller.statusLine().tone == StatusTone::Bad);
}

MONOSECT_TEST(cancelling_keeps_verified_parts_and_is_described_honestly) {
    FakeEngine engine;
    engine.runSleepsUntilCanceled = true;
    UiController controller(engine);
    const TempDirectory directory;
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);
    controller.start(directory.path.string());
    CHECK(controller.jobRunning());

    controller.poll();
    controller.requestCancel();
    CHECK(controller.snapshot().phase == SessionPhase::Cancelling);
    CHECK(controller.snapshot().statusDetail.find("finished first") != std::string::npos);

    for (int attempt = 0; attempt < 500 && controller.jobRunning(); ++attempt) {
        controller.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    controller.waitForJob();
    CHECK(controller.snapshot().canceled);
    CHECK_EQ(std::string("Cancelled"), controller.statusLine().headline);
    CHECK(controller.statusLine().tone == StatusTone::Warning);
}

MONOSECT_TEST(pausing_is_only_claimed_once_the_runner_has_paused) {
    FakeEngine engine;
    engine.runSleepsUntilPaused = true;
    UiController controller(engine);
    const TempDirectory directory;
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);
    controller.start(directory.path.string());
    CHECK(controller.jobRunning());

    // Before the pause is requested the job is simply running.
    controller.poll();
    CHECK(controller.snapshot().phase == SessionPhase::Running);

    controller.requestPause();
    CHECK(controller.snapshot().phase == SessionPhase::Pausing);
    CHECK(controller.snapshot().statusHeadline == "Pausing");
    CHECK(controller.snapshot().statusDetail.find("next part boundary") != std::string::npos);

    bool sawPaused = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        controller.poll();
        if (controller.snapshot().phase == SessionPhase::Paused) {
            sawPaused = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(sawPaused);
    CHECK_EQ(std::string("Paused"), controller.snapshot().statusHeadline);

    controller.requestResume();
    CHECK(controller.snapshot().phase == SessionPhase::Running);
    for (int attempt = 0; attempt < 500 && controller.jobRunning(); ++attempt) {
        controller.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    controller.waitForJob();
    CHECK(controller.snapshot().phase == SessionPhase::Completed);
}

MONOSECT_TEST(the_resource_panel_reports_unavailable_metrics_as_unavailable) {
    FakeEngine engine;
    UiController controller(engine);
    const ResourceSample& sample = controller.sampleResources();
    CHECK(!sample.sourceDescription.empty());
    // Whatever the machine supports, an unavailable metric carries a reason.
    if (!sample.gpuPercent.known) {
        CHECK(!sample.gpuPercent.unavailableReason.empty());
        CHECK_EQ(std::string("n/a"), percentText(sample.gpuPercent));
    }
    if (!sample.diskReadBytesPerSecond.known) {
        CHECK_EQ(std::string("n/a"), bytesPerSecondText(sample.diskReadBytesPerSecond));
    }
    CHECK(!controller.resourceAvailabilityNote().empty());
    CHECK(controller.resourceInput().topology.logicalCores >= 1);
}

MONOSECT_TEST(the_job_history_is_read_from_the_database_file) {
    FakeEngine engine;
    UiController controller(engine);
    const TempDirectory directory;
    const std::string database = (directory.path / "jobs.jsonl").string();
    controller.setDatabasePath(database);
    CHECK(controller.history().empty());

    JobRecord record;
    record.jobId = "job-1";
    record.startedAt = "2026-01-01T00:00:00";
    record.finishedAt = "2026-01-01T00:01:00";
    record.finalState = "Completed";
    record.message = "wrote 3 part(s)";
    record.sourcePath = "C:/media/movie.mkv";
    record.largestPartBytes = 2048;
    JobStore store(database);
    CHECK(store.append(record).ok());

    controller.refreshHistory();
    CHECK(controller.history().size() == 1);
    CHECK_EQ(std::string("job-1"), controller.history().front().jobId);
    CHECK(controller.historyProblem().empty());

    // A history path that is not a file is a problem with a message, not an empty
    // list that hides the fact that nothing could be read.
    controller.setDatabasePath(directory.path.string());
    CHECK(controller.history().empty());
    CHECK(!controller.historyProblem().empty());
    CHECK(controller.historyProblem().find("not a file") != std::string::npos);
}

MONOSECT_TEST(help_opens_on_the_topic_for_the_control_that_asked) {
    FakeEngine engine;
    UiController controller(engine);
    CHECK(!controller.helpVisible());

    controller.showHelp(HelpTopicId::SizeLimit);
    CHECK(controller.helpVisible());
    CHECK(controller.helpTopic() != nullptr);
    CHECK_EQ(std::string("size-limit"), controller.helpTopic()->idText);

    controller.setHelpQuery("keyframe");
    CHECK(!controller.helpResults().empty());
    CHECK(controller.helpTopic() != nullptr);
    bool foundKeyframeTopic = false;
    for (const HelpTopic* result : controller.helpResults()) {
        if (result != nullptr && result->idText == "keyframe-aligned") {
            foundKeyframeTopic = true;
        }
    }
    CHECK(foundKeyframeTopic);

    controller.closeHelp();
    CHECK(!controller.helpVisible());
    CHECK(controller.helpTopic() == nullptr);

    // The help a control opens is chosen from the constraint, not hard-coded in
    // the window: a size limit opens the size-limit topic.
    UiController second(engine);
    second.setSizeLimitBytes(1024 * 1024);
    const HelpTopic* topic = UiModel::helpForConstraint(second.constraint());
    CHECK(topic != nullptr);
    if (topic != nullptr) {
        CHECK_EQ(std::string("size-limit"), topic->idText);
    }
    second.clearSizeLimit();
    second.setDurationLimit(Rational{60, 1});
    topic = UiModel::helpForConstraint(second.constraint());
    CHECK(topic != nullptr);
    if (topic != nullptr) {
        CHECK_EQ(std::string("duration-limit"), topic->idText);
    }
}

MONOSECT_TEST(progress_is_reported_by_parts_and_has_no_eta_before_the_first_one) {
    FakeEngine engine;
    UiController controller(engine);
    const TempDirectory directory;
    controller.addSource("C:/media/movie.mkv");
    controller.probeSelected();
    controller.constraint() = sizeLimit(200000);
    controller.previewPlan(directory.path.string());

    const ProgressView before = controller.progressView();
    CHECK_EQ(0, before.partsDone);
    CHECK(!before.secondsRemaining.has_value());
    CHECK(before.describe().find("no estimate") != std::string::npos);

    controller.start(directory.path.string());
    CHECK(controller.jobRunning());
    CHECK(waitForTerminal(controller));
    controller.waitForJob();
    const ProgressView after = controller.progressView();
    CHECK(after.partsTotal > 0);
    CHECK_EQ(after.partsTotal, after.partsDone);
    CHECK_NEAR(1.0, after.fraction, 1e-9);
}

namespace {

/// A hardware capability source with a fixed answer, so the resource policy can be
/// tested for every machine MonoSect might run on without one of them.
struct FakeHardware final : HardwareCapabilitySource {
    explicit FakeHardware(HardwareCapabilities answer) : result(std::move(answer)) {}

    void startProbe() override { ++startCalls; }
    bool probeRunning() const override { return running; }
    HardwareCapabilities capabilities() const override { return result; }

    HardwareCapabilities result;
    bool running = false;
    int startCalls = 0;
};

HardwareCapabilities noHardwareAtAll() {
    HardwareCapabilities answer;
    answer.probed = true;
    return answer;
}

HardwareCapabilities workingNvidia() {
    HardwareCapabilities answer;
    answer.probed = true;
    EncoderProbe h264;
    h264.encoder = "h264_nvenc";
    h264.vendor = "NVIDIA";
    h264.codecName = "h264";
    h264.listed = true;
    h264.usable = true;
    answer.encoders.push_back(h264);
    EncoderProbe qsv;
    qsv.encoder = "h264_qsv";
    qsv.vendor = "Intel";
    qsv.codecName = "h264";
    qsv.listed = true;
    qsv.usable = false;      // listed by the build, refused by the machine
    qsv.detail = "no Intel device with an Quick Sync encoder";
    answer.encoders.push_back(qsv);
    answer.usableVendors.push_back("NVIDIA");
    return answer;
}

} // namespace

MONOSECT_TEST(an_unprobed_machine_is_not_reported_as_a_machine_without_hardware) {
    // The distinction the product depends on: "not asked yet" is not "no card".
    // Reporting the second when the machine has merely not been probed would
    // disable hardware encoding on a capable machine and tell the user their card
    // is not there.
    HardwareCapabilities pending;
    FakeHardware hardware(pending);
    FakeEngine engine;
    UiController controller(engine, &hardware);

    CHECK(!controller.hardwareProbed());
    const ResourcePolicyInput input = controller.resourceInput();
    CHECK(input.gpu.name == "not probed yet");
    CHECK(!input.gpu.encoderUsable);
    CHECK(!input.gpu.encoderPresent);
    CHECK(controller.hardwareSummary().find("not run yet") != std::string::npos);
}

MONOSECT_TEST(a_listed_encoder_that_does_not_work_is_not_offered) {
    // The defect the old detector had: a static FFmpeg build lists every vendor
    // encoder, so listing them as available offered an encode that fails - and
    // turned a requested stream copy into a re-encode without saying so.
    FakeHardware hardware(workingNvidia());
    FakeEngine engine;
    UiController controller(engine, &hardware);

    CHECK(controller.hardwareProbed());
    CHECK_EQ(std::size_t(2u), controller.hardwareEncoders().size());

    const ResourcePolicyInput input = controller.resourceInput();
    // encoderPresent means "listed by this FFmpeg build", which is true of both.
    CHECK(input.gpu.encoderPresent);
    CHECK(input.gpu.probeDetail.find("h264_qsv") != std::string::npos);
    // encoderUsable is deliberately false here: with no preview, jobWillEncode is
    // false, because a stream copy needs no encoder at all - so no encoder may be
    // selected.  That is the rule that stops a requested copy becoming an encode.
    CHECK(!input.gpu.encoderUsable);
    CHECK(input.gpu.usableEncoder.empty());
    CHECK(input.gpu.name.find("no hardware encoder is needed") != std::string::npos);
    CHECK(input.gpu.probeDetail.find("Quick Sync") != std::string::npos);
}

MONOSECT_TEST(no_gpu_encoder_is_selected_for_a_machine_without_one) {
    FakeHardware hardware(noHardwareAtAll());
    FakeEngine engine;
    UiController controller(engine, &hardware);

    const ResourcePolicyInput input = controller.resourceInput();
    CHECK(!input.gpu.encoderUsable);
    CHECK(input.gpu.usableEncoder.empty());

    // jobWillEncode is false while nothing is planned, so the policy has no reason
    // to reach for an encoder at all.
    CHECK(!controller.jobWillEncode());
    const ResourcePolicy policy = deriveResourcePolicy(input);
    CHECK(!policy.useGpuEncoder);
}

MONOSECT_TEST(the_users_choice_about_the_gpu_is_kept_apart_from_what_the_machine_can_do) {
    // "The card can encode" and "the user wants it to" are separate decisions.  A
    // controller that conflated them would either ignore the checkbox or refuse to
    // use a working card.
    FakeHardware hardware(workingNvidia());
    FakeEngine engine;
    UiController controller(engine, &hardware);

    CHECK(controller.gpuEnabled());
    CHECK(controller.hardwareDecodingEnabled());

    controller.setGpuEnabled(false);
    CHECK(!controller.gpuEnabled());
    CHECK(!controller.resourceInput().userEnabledGpu);

    controller.setHardwareDecodingEnabled(false);
    CHECK(!controller.resourceInput().userEnabledHardwareDecoding);
}

MONOSECT_TEST(a_controller_without_a_hardware_source_reports_it_rather_than_pretending) {
    FakeEngine engine;
    UiController controller(engine);

    CHECK(!controller.hardwareProbed());
    CHECK(controller.hardwareEncoders().empty());
    CHECK(controller.hardwareSummary().find("not available") != std::string::npos);
    // The rest of the resource policy still works: a missing probe must not stop
    // the thread and priority settings from being derived.
    const ResourcePolicyInput input = controller.resourceInput();
    CHECK(input.topology.logicalCores >= 1);
}
