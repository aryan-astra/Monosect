#include "engine/JobRunner.hpp"

#include "core/ProbeParsing.hpp"
#include "engine/FfmpegArgs.hpp"
#include "engine/JobStore.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <thread>

namespace monosect::core {
namespace {

constexpr double kProgressThrottleSeconds = 0.15;
constexpr double kClosenessThreshold = 0.02;        // 2 % of the limit
constexpr ByteCount kClosenessThresholdBytes = 1 * kMiB;

} // namespace

ByteCount JobOutcome::largestPublishedBytes() const {
    ByteCount largest = 0;
    for (const auto& part : parts) {
        largest = std::max(largest, part.fileSizeBytes);
    }
    return largest;
}

Rational JobOutcome::longestPublishedDuration() const {
    Rational longest{0, 1};
    for (const auto& part : parts) {
        if (compareRational(part.duration, longest) > 0) {
            longest = part.duration;
        }
    }
    return longest;
}

std::string JobOutcome::summary() const {
    std::string text = message;
    if (!publishedFiles.empty()) {
        text += " Published: ";
        for (std::size_t i = 0; i < publishedFiles.size() && i < 4; ++i) {
            if (i > 0) {
                text += ", ";
            }
            text += publishedFiles[i].filename().string();
        }
        if (publishedFiles.size() > 4) {
            text += " ... (" + std::to_string(publishedFiles.size()) + " files)";
        }
        text += ".";
    }
    return text;
}

JobRunner::JobRunner(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath)
    : prober_(prober), runner_(runner), ffmpegPath_(std::move(ffmpegPath)) {}

Result<SplitPlan> JobRunner::planOnly(const JobRequest& request, const JobEvents& events) {
    const auto log = [&events](const std::string& line) {
        if (events.onLog) {
            events.onLog(line);
        }
    };
    prober_.clearCache();
    auto snapshot = prober_.probeAll(request.sourcePath);
    if (!snapshot.has_value()) {
        sourceValid_ = false;
        return failure<SplitPlan>(snapshot.error());
    }
    source_ = snapshot.value();
    sourceValid_ = true;

    const ConstraintReport report = validateConstraint(request.constraint, snapshot.value().container);
    if (!report.valid) {
        std::string message = "This split cannot be planned:";
        std::string remedy;
        for (const auto& issue : report.issues) {
            if (issue.severity != ConstraintIssue::Severity::Error) {
                continue;
            }
            message += " " + issue.message;
            if (remedy.empty()) {
                remedy = issue.remedy;
            }
        }
        return Error{ErrorCode::ConstraintInvalid, std::move(message),
                     describeConstraint(request.constraint) + "; " + snapshot.value().container.describe(), remedy};
    }

    SizeModel sizeModel;
    sizeModel.seedFromSource(snapshot.value().container.formatNames, snapshot.value().container.fileSize,
                             snapshot.value().packets.totalPayloadBytes());

    SplitPlanner planner(snapshot.value().container, snapshot.value().packets, request.constraint, request.naming,
                         sizeModel, request.plannerSettings);
    auto plan = planner.plan(request.outputDirectory);
    if (!plan.has_value()) {
        return plan;
    }

    for (const auto& issue : report.issues) {
        if (issue.severity == ConstraintIssue::Severity::Warning) {
            plan.value().warnings.push_back(issue.message);
        }
    }
    for (const std::string& warning : snapshot.value().container.warnings) {
        plan.value().warnings.push_back(warning);
    }
    if (planner.stats().candidateEvaluations > 0) {
        log("planned " + std::to_string(plan.value().partCount()) + " part(s) from the packet index (" +
            std::to_string(planner.stats().candidateEvaluations) + " candidate positions evaluated)");
    }
    if (events.onPlanReady) {
        events.onPlanReady(plan.value());
    }
    return plan;
}

Error JobRunner::interpretFfmpegFailure(const ProcessResult& result, const std::filesystem::path& output,
                                        const std::string& commandLine) const {
    std::string detail;
    for (const std::string& line : result.stderrTail) {
        if (detail.size() > 700) {
            break;
        }
        detail += line;
        detail += ' ';
    }
    if (detail.empty()) {
        detail = "ffmpeg exited with code " + std::to_string(result.exitCode);
    }
    std::string lower = detail;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (!result.launched) {
        return Error{ErrorCode::FfmpegBinaryMissing, "ffmpeg could not be started.",
                     result.launchError.empty() ? commandLine : result.launchError,
                     "Install the FFmpeg runtime from Settings, or point MonoSect at an existing ffmpeg.exe."};
    }
    if (lower.find("no space left") != std::string::npos || lower.find("disk full") != std::string::npos) {
        return Error{ErrorCode::DiskSpaceInsufficient, "The output disk ran out of space.",
                     output.parent_path().string(),
                     "Free space on the output drive, or choose another output folder."};
    }
    if (lower.find("permission denied") != std::string::npos ||
        lower.find("access is denied") != std::string::npos) {
        return Error{ErrorCode::FileAccessDenied, "FFmpeg was not allowed to write the output file.",
                     output.string(), "Choose a folder you can write to, or close the program holding the file."};
    }
    if (lower.find("invalid data found") != std::string::npos ||
        lower.find("moov atom not found") != std::string::npos ||
        lower.find("could not find codec parameters") != std::string::npos) {
        return Error{ErrorCode::UnsupportedInputFormat, "FFmpeg could not read a part of the source file.",
                     detail, "The file may be damaged or may still be downloading."};
    }
    if (lower.find("unknown encoder") != std::string::npos) {
        return Error{ErrorCode::UnsupportedOperation,
                     "The FFmpeg build in use has no encoder for this format.",
                     detail, "Use a copy mode, or install the standard MonoSect FFmpeg runtime."};
    }
    return Error{ErrorCode::ProcessFailed, "Writing a part failed.", detail + " command: " + commandLine,
                 "The part was discarded; the job can be retried."};
}

std::int64_t JobRunner::packetCountInRange(int streamIndex, Rational start, Rational end) const {
    if (!sourceValid_) {
        return -1;
    }
    const StreamPacketTrack* track = source_.packets.track(streamIndex);
    if (track == nullptr) {
        return -1;
    }
    return track->packetsInRange(start, end);
}

Result<std::filesystem::path> JobRunner::copyRange(const SplitPlan& plan, const std::filesystem::path& outputPath,
                                                   Rational start, Rational end,
                                                   const std::vector<StreamTrimSpec>& trims,
                                                   const JobRequest& request, const JobEvents& events,
                                                   bool startIsKeyframe, std::vector<std::string>* notes) {
    const std::filesystem::path workDirectory = outputPath.parent_path();
    const std::string stem = outputPath.filename().string();
    const std::string containerFormat = containerNameForExtension(plan.outputExtension);

    std::vector<std::filesystem::path> trimmedFiles;
    std::vector<std::string> trimNotes;
    for (const StreamTrimSpec& trim : trims) {
        if (trim.packetCount == 0) {
            // The stream has no packet in this range.  A part cannot contain a
            // stream that has no data in it; the plan records this and the
            // verifier expects the stream to be absent.
            trimNotes.push_back("stream " + std::to_string(trim.streamIndex) +
                                " has no packet between " + formatDuration(start) + " and " +
                                formatDuration(end) + ", so this part does not carry it");
            continue;
        }
        const std::filesystem::path trimmed =
            workDirectory / (stem + ".stream" + std::to_string(trim.streamIndex) + ".mkv");

        StreamTrimRequest trimRequest;
        trimRequest.input = plan.sourcePath;
        trimRequest.output = trimmed.string();
        trimRequest.inputStreamIndex = trim.streamIndex;
        trimRequest.containerFormat = "matroska";
        trimRequest.threads = request.ffmpegThreads;
        if (trim.packetCount > 0) {
            trimRequest.packetCount = trim.packetCount;
            trimRequest.hasPacketCount = true;
        } else {
            trimRequest.hasPacketCount = false;
        }
        // Seeking: a video stream is only exact when FFmpeg is allowed to seek
        // to the keyframe itself (input-side).  Output-side seeking is the exact
        // one for every other stream, and it is *wrong* for video: measured on
        // ffmpeg 7.0.2, `-i in -ss 2` on an H.264 stream skipped twice the seek
        // distance and kept the timestamps of the requested position, which is a
        // silent content shift.  See FfmpegArgs.hpp.
        if (trim.kind == StreamKind::Video && startIsKeyframe) {
            trimRequest.seekMode = StreamTrimRequest::SeekMode::InputKeyframe;
            trimRequest.start = start;
        } else if (trim.kind == StreamKind::Video) {
            return Error{ErrorCode::SmartCutUnavailable,
                         "A video stream can only be copied when the part starts at one of its keyframes.",
                         "stream " + std::to_string(trim.streamIndex) + ", start " + formatDuration(start),
                         "Use a mode that can re-encode the boundary, or choose keyframe-aligned copying."};
        } else {
            trimRequest.seekMode = StreamTrimRequest::SeekMode::OutputPrecise;
            trimRequest.start = start;
        }

        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = trimStreamArgs(trimRequest);
        spec.lowPriority = request.lowPriority;
        ProcessCallbacks callbacks;
        callbacks.shouldCancel = events.shouldCancel;
        const std::string commandLine = formatCommandLineForDisplay(spec);
        const ProcessResult result = runner_.run(spec, callbacks);
        if (result.canceled) {
            return Error{ErrorCode::ProcessCanceled, "Writing this part was cancelled.", commandLine};
        }
        if (!result.launched || result.exitCode != 0) {
            return failure<std::filesystem::path>(interpretFfmpegFailure(result, trimmed, commandLine));
        }
        if (events.onLog) {
            events.onLog("stream " + std::to_string(trim.streamIndex) + ": " +
                         std::to_string(trim.packetCount < 0 ? 0 : trim.packetCount) +
                         " packet(s) trimmed with " +
                         (trimRequest.seekMode == StreamTrimRequest::SeekMode::InputKeyframe ? "an input keyframe seek"
                                                                                             : "an exact output seek"));
        }
        trimmedFiles.push_back(trimmed);
    }

    if (trimmedFiles.empty()) {
        return Error{ErrorCode::PlanFailed, "This part would contain no stream at all.",
                     "range " + formatDuration(start) + " - " + formatDuration(end),
                     "Check the plan; every part must carry at least one stream."};
    }

    std::filesystem::path resultPath = outputPath;
    // The trimmed intermediates are always Matroska (it is the one container that
    // holds every codec unchanged), so a single-stream part can only be renamed
    // into place when the plan asks for Matroska as well.  Renaming in the other
    // cases would publish Matroska bytes under a .mp4 name - the file would open,
    // but the container the user asked for and the extension they see would both
    // be wrong, and a later merge would have to guess.  A one-stream remux is
    // cheap, so it always happens unless the containers already agree.
    const bool singleStreamInPlannedContainer = trimmedFiles.size() == 1 && containerFormat == "matroska";
    if (singleStreamInPlannedContainer) {
        std::error_code renameError;
        std::filesystem::rename(trimmedFiles.front(), resultPath, renameError);
        if (renameError) {
            return Error{ErrorCode::OutputWriteFailed, "The written part could not be moved into place.",
                         renameError.message() + " (" + trimmedFiles.front().string() + ")",
                         "Check that the output folder is writable and on the same drive."};
        }
    } else {
        MuxStreamsRequest mux;
        mux.output = resultPath.string();
        for (const auto& file : trimmedFiles) {
            mux.inputs.push_back(file.string());
        }
        mux.metadataSource = plan.sourcePath;
        mux.containerFormat = containerFormat;
        // Every trimmed stream is expressed relative to the same origin (the
        // start of this part), so the muxer must not re-base them one by one:
        // that is what would make a stream whose first packet is not exactly on
        // the origin - audio, almost always - land in the wrong place.  See
        // MuxStreamsRequest::preserveTimestamps.
        mux.preserveTimestamps = true;
        mux.threads = request.ffmpegThreads;
        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = muxStreamsArgs(mux);
        spec.lowPriority = request.lowPriority;
        ProcessCallbacks callbacks;
        callbacks.shouldCancel = events.shouldCancel;
        const std::string commandLine = formatCommandLineForDisplay(spec);
        const ProcessResult result = runner_.run(spec, callbacks);
        if (result.canceled) {
            return Error{ErrorCode::ProcessCanceled, "Writing this part was cancelled.", commandLine};
        }
        if (!result.launched || result.exitCode != 0) {
            return failure<std::filesystem::path>(interpretFfmpegFailure(result, resultPath, commandLine));
        }
        std::error_code removeError;
        for (const auto& file : trimmedFiles) {
            std::filesystem::remove(file, removeError);
        }
    }
    if (notes != nullptr) {
        for (const std::string& note : trimNotes) {
            notes->push_back(note);
        }
    }
    return resultPath;
}

Result<JobRunner::RenderResult> JobRunner::renderCopy(const SplitPlan& plan, const SegmentPlan& segment,
                                                      const std::filesystem::path& temporaryPath,
                                                      const JobRequest& request, const JobEvents& events) {
    std::vector<StreamTrimSpec> trims;
    for (const int streamIndex : segment.copiedStreamIndexes) {
        StreamTrimSpec trim;
        trim.streamIndex = streamIndex;
        if (const StreamModel* stream = source_.container.stream(streamIndex); stream != nullptr) {
            trim.kind = stream->kind;
        }
        trim.packetCount = packetCountInRange(streamIndex, segment.startSeconds, segment.endSeconds);
        if (trim.packetCount < 0) {
            return Error{ErrorCode::PlanFailed,
                         "The packet count of a stream could not be determined, so this part cannot be copied "
                         "exactly.",
                         "stream " + std::to_string(streamIndex) + " of " + plan.sourcePath,
                         "Re-probe the source; if the file is damaged, remux it first."};
        }
        trims.push_back(trim);
    }

    auto copied = copyRange(plan, temporaryPath, segment.startSeconds, segment.endSeconds, trims, request, events,
                            segment.isCopy(), nullptr);
    if (!copied.has_value()) {
        return failure<RenderResult>(copied.error());
    }

    RenderResult rendered;
    rendered.file = copied.value();
    rendered.outcome.index = segment.index;
    rendered.outcome.startSeconds = segment.startSeconds;
    rendered.outcome.endSeconds = segment.endSeconds;
    rendered.outcome.method = segment.method;
    rendered.outcome.wrote = true;
    return rendered;
}

Result<JobRunner::RenderResult> JobRunner::renderSmartCut(const SplitPlan& plan, const SegmentPlan& segment,
                                                          const std::filesystem::path& temporaryPath,
                                                          const JobRequest& request, const JobEvents& events) {
    if (segment.reencodeRegions.empty()) {
        return renderCopy(plan, segment, temporaryPath, request, events);
    }
    const ReencodeRegion& head = segment.reencodeRegions.front();
    const std::filesystem::path headPath = temporaryPath.string() + ".head";
    const std::filesystem::path bodyPath = temporaryPath.string() + ".body";
    const std::filesystem::path listPath = temporaryPath.string() + ".list";

    // 1. The re-encoded head: from the exact start of the part to the next
    //    keyframe.  It is encoded with the source's own codec and parameters so
    //    that it can be concatenated with the copied body.
    SegmentRenderRequest headRender;
    headRender.input = plan.sourcePath;
    headRender.output = headPath.string();
    headRender.start = head.startSeconds;
    headRender.end = head.endSeconds;
    headRender.containerFormat = containerNameForExtension(plan.outputExtension);
    headRender.threads = request.ffmpegThreads;
    headRender.lowPriority = request.lowPriority;
    headRender.copyMetadata = false;
    for (std::size_t i = 0; i < segment.reencodedStreamIndexes.size(); ++i) {
        SegmentRenderRequest::ReencodedStream stream;
        stream.index = segment.reencodedStreamIndexes[i];
        if (i < segment.reencodedCodecNames.size()) {
            stream.codecName = segment.reencodedCodecNames[i];
        }
        if (i < segment.reencodedKinds.size()) {
            stream.kind = segment.reencodedKinds[i];
        }
        if (i < segment.reencodedBitRates.size()) {
            stream.bitRate = segment.reencodedBitRates[i];
        }
        if (stream.codecName.empty()) {
            return Error{ErrorCode::UnsupportedOperation,
                         "Smart cut cannot re-encode a stream without knowing its codec.",
                         "the plan does not describe the re-encoded stream",
                         "Choose keyframe-aligned copying instead."};
        }
        headRender.reencodedStreams.push_back(stream);
    }
    for (const int index : segment.copiedStreamIndexes) {
        headRender.copiedStreams.push_back(index);
    }

    ProcessSpec headSpec;
    headSpec.program = ffmpegPath_;
    headSpec.arguments = renderSegmentArgs(headRender);
    ProcessCallbacks callbacks;
    callbacks.shouldCancel = events.shouldCancel;
    callbacks.onStderrLine = [&events](const std::string& line) {
        if (events.onLog) {
            events.onLog("ffmpeg: " + line);
        }
    };
    const ProcessResult headResult = runner_.run(headSpec, callbacks);
    if (headResult.canceled) {
        return Error{ErrorCode::ProcessCanceled, "Writing this part was cancelled.",
                     formatCommandLineForDisplay(headSpec)};
    }
    if (!headResult.launched || headResult.exitCode != 0) {
        return interpretFfmpegFailure(headResult, headPath, formatCommandLineForDisplay(headSpec));
    }

    // 2. The copied body: from the keyframe to the end of the part.
    SegmentRenderRequest bodyRender;
    bodyRender.input = plan.sourcePath;
    bodyRender.output = bodyPath.string();
    bodyRender.start = head.endSeconds;
    bodyRender.end = segment.endSeconds;
    bodyRender.copiedStreams = segment.copiedStreamIndexes;
    bodyRender.containerFormat = containerNameForExtension(plan.outputExtension);
    bodyRender.threads = request.ffmpegThreads;
    bodyRender.lowPriority = request.lowPriority;

    ProcessSpec bodySpec;
    bodySpec.program = ffmpegPath_;
    bodySpec.arguments = renderSegmentArgs(bodyRender);
    const ProcessResult bodyResult = runner_.run(bodySpec, callbacks);
    if (bodyResult.canceled) {
        return Error{ErrorCode::ProcessCanceled, "Writing this part was cancelled.",
                     formatCommandLineForDisplay(bodySpec)};
    }
    if (!bodyResult.launched || bodyResult.exitCode != 0) {
        return interpretFfmpegFailure(bodyResult, bodyPath, formatCommandLineForDisplay(bodySpec));
    }

    // 3. Concatenate without re-encoding.
    const Status listStatus = writeConcatList(listPath, {headPath, bodyPath});
    if (!listStatus.ok()) {
        return failure<RenderResult>(listStatus.error);
    }
    ProcessSpec concatSpec;
    concatSpec.program = ffmpegPath_;
    concatSpec.arguments = concatArgs(listPath.string(), temporaryPath.string(),
                                      containerNameForExtension(plan.outputExtension));
    const ProcessResult concatResult = runner_.run(concatSpec, callbacks);
    std::error_code error;
    std::filesystem::remove(headPath, error);
    std::filesystem::remove(bodyPath, error);
    std::filesystem::remove(listPath, error);
    if (concatResult.canceled) {
        return Error{ErrorCode::ProcessCanceled, "Writing this part was cancelled.",
                     formatCommandLineForDisplay(concatSpec)};
    }
    if (!concatResult.launched || concatResult.exitCode != 0) {
        return interpretFfmpegFailure(concatResult, temporaryPath, formatCommandLineForDisplay(concatSpec));
    }

    RenderResult renderResult;
    renderResult.file = temporaryPath;
    renderResult.outcome.index = segment.index;
    renderResult.outcome.startSeconds = segment.startSeconds;
    renderResult.outcome.endSeconds = segment.endSeconds;
    renderResult.outcome.method = segment.method;
    renderResult.outcome.wrote = true;
    renderResult.outcome.fileSizeBytes = static_cast<ByteCount>(std::filesystem::file_size(temporaryPath, error));
    if (error) {
        return Error{ErrorCode::OutputWriteFailed, "The written part could not be found afterwards.",
                     temporaryPath.string(), "Check the output drive."};
    }
    return renderResult;
}

Result<JobRunner::RenderResult> JobRunner::renderReencode(const SplitPlan& plan, const SegmentPlan& segment,
                                                          const std::filesystem::path& temporaryPath,
                                                          const JobRequest& request, const JobEvents& events) {
    SegmentRenderRequest render;
    render.input = plan.sourcePath;
    render.output = temporaryPath.string();
    render.start = segment.startSeconds;
    render.end = segment.endSeconds;
    render.copiedStreams = segment.copiedStreamIndexes;
    render.containerFormat = containerNameForExtension(plan.outputExtension);
    render.threads = request.ffmpegThreads;
    render.lowPriority = request.lowPriority;
    render.startIsKeyframe = false;
    for (std::size_t i = 0; i < segment.reencodedStreamIndexes.size(); ++i) {
        SegmentRenderRequest::ReencodedStream stream;
        stream.index = segment.reencodedStreamIndexes[i];
        if (i < segment.reencodedCodecNames.size()) {
            stream.codecName = segment.reencodedCodecNames[i];
        }
        stream.kind = i < segment.reencodedKinds.size() ? segment.reencodedKinds[i] : StreamKind::Unknown;
        stream.bitRate = i < segment.reencodedBitRates.size() ? segment.reencodedBitRates[i] : 0;
        render.reencodedStreams.push_back(stream);
    }

    ProcessSpec spec;
    spec.program = ffmpegPath_;
    spec.arguments = renderSegmentArgs(render);
    spec.lowPriority = request.lowPriority;
    const std::string commandLine = formatCommandLineForDisplay(spec);

    ProcessCallbacks callbacks;
    callbacks.shouldCancel = events.shouldCancel;
    callbacks.onStderrLine = [&events](const std::string& line) {
        if (events.onLog) {
            events.onLog("ffmpeg: " + line);
        }
    };
    const ProcessResult result = runner_.run(spec, callbacks);
    if (result.canceled) {
        return Error{ErrorCode::ProcessCanceled, "Writing this part was cancelled.", commandLine};
    }
    if (!result.launched || result.exitCode != 0) {
        return interpretFfmpegFailure(result, temporaryPath, commandLine);
    }
    RenderResult renderResult;
    renderResult.file = temporaryPath;
    renderResult.outcome.index = segment.index;
    renderResult.outcome.startSeconds = segment.startSeconds;
    renderResult.outcome.endSeconds = segment.endSeconds;
    renderResult.outcome.method = segment.method;
    renderResult.outcome.wrote = true;
    std::error_code error;
    renderResult.outcome.fileSizeBytes = static_cast<ByteCount>(std::filesystem::file_size(temporaryPath, error));
    if (error) {
        return Error{ErrorCode::OutputWriteFailed, "The written part could not be found afterwards.",
                     temporaryPath.string(), "Check the output drive."};
    }
    return renderResult;
}

Result<JobRunner::RenderResult> JobRunner::renderPart(const SplitPlan& plan, const SegmentPlan& segment,
                                                      const std::filesystem::path& temporaryPath,
                                                      const JobRequest& request, const JobEvents& events) {
    switch (segment.method) {
        case SegmentMethod::Copy:
            return renderCopy(plan, segment, temporaryPath, request, events);
        case SegmentMethod::SmartCut:
            return renderSmartCut(plan, segment, temporaryPath, request, events);
        case SegmentMethod::ReEncode:
            return renderReencode(plan, segment, temporaryPath, request, events);
    }
    return Error{ErrorCode::Internal, "The plan contains an unknown part method.", ""};
}

Result<JobOutcome> JobRunner::run(const JobRequest& request, const JobEvents& events) {
    // Every terminal state - completed, failed, cancelled - is remembered, so
    // that a later run can tell what was already written and verified.  Recording
    // is best effort by design: a history file that cannot be written must not
    // destroy a job that succeeded (TODO 143).
    auto outcome = runInternal(request, events);
    if (outcome.has_value()) {
        recordJob(request, events, outcome.value());
    } else {
        recordFailure(request, events, outcome.error());
    }
    return outcome;
}

void JobRunner::recordJob(const JobRequest& request, const JobEvents& events, JobOutcome& outcome) {
    const auto log = [&outcome, &events](const std::string& line) {
        outcome.log.push_back(line);
        if (events.onLog) {
            events.onLog(line);
        }
    };
    if (!request.recordHistory) {
        return;
    }
    JobStore store = request.databasePath.empty() ? JobStore::defaultStore() : JobStore(request.databasePath);
    JobRecord record;
    record.jobId = outcome.jobId;
    record.startedAt = jobStartedAt_;
    record.finishedAt = currentTimestamp();
    record.finalState = jobStateName(outcome.finalState);
    record.message = outcome.message;
    record.sourcePath = request.sourcePath;
    record.sourceBytes = sourceValid_ ? source_.container.fileSize : 0;
    record.sourceDuration = sourceValid_ && source_.container.durationResolved.known()
                                ? *source_.container.durationResolved.seconds
                                : Rational{0, 1};
    record.outputDirectory = request.outputDirectory;
    record.constraintDescription = describeConstraint(request.constraint);
    record.modeDescription =
        std::string(preservationModeName(request.constraint.preservation)) + ", " +
        precisionPolicyName(request.constraint.precision) + " precision";
    record.appVersion = request.appVersion;
    record.ffmpegVersion = request.ffmpegVersion;
    record.ffprobeVersion = request.ffprobeVersion;
    record.planJson = outcome.plan.toJson();
    record.parts = outcome.parts;
    record.warnings = outcome.warnings;
    record.largestPartBytes = outcome.largestPublishedBytes();
    record.longestPartDuration = outcome.longestPublishedDuration();
    record.planningSeconds = outcome.planningSeconds;
    record.executionSeconds = outcome.executionSeconds;
    record.verificationSeconds = outcome.verificationSeconds;
    record.partsRewritten = outcome.partsRewrittenAfterFailure + outcome.partsRewrittenForCloseness;

    const Status status = store.append(record);
    if (status.ok()) {
        log("job " + outcome.jobId + " recorded in " + store.path().string());
    } else {
        const std::string warning = "The job record could not be written to " + store.path().string() + ": " +
                                    status.error.message + " The split itself is unaffected.";
        outcome.warnings.push_back(warning);
        log(warning);
    }
}

void JobRunner::recordFailure(const JobRequest& request, const JobEvents& events, const Error& error) {
    (void)events;
    if (!request.recordHistory) {
        return;
    }
    JobStore store = request.databasePath.empty() ? JobStore::defaultStore() : JobStore(request.databasePath);
    JobRecord record;
    record.jobId = currentJobId_;
    record.startedAt = jobStartedAt_;
    record.finishedAt = currentTimestamp();
    record.finalState = jobStateName(JobState::Failed);
    record.message = error.message;
    record.sourcePath = request.sourcePath;
    record.outputDirectory = request.outputDirectory;
    record.constraintDescription = describeConstraint(request.constraint);
    record.appVersion = request.appVersion;
    record.warnings.push_back(error.toString());
    (void)store.append(record);
}

Result<JobOutcome> JobRunner::runInternal(const JobRequest& request, const JobEvents& events) {
    JobOutcome outcome;
    outcome.jobId = makeJobId();
    currentJobId_ = outcome.jobId;
    jobStartedAt_ = currentTimestamp();

    const auto setState = [&events, &outcome](JobState state, const std::string& message) {
        outcome.finalState = state;
        if (events.onState) {
            events.onState(state, message);
        }
    };
    const auto log = [&outcome, &events](const std::string& line) {
        outcome.log.push_back(line);
        if (events.onLog) {
            events.onLog(line);
        }
    };
    const auto canceled = [&events]() { return events.shouldCancel && events.shouldCancel(); };
    const auto pauseRequested = [&events]() { return events.shouldPause && events.shouldPause(); };

    setState(JobState::Planning, "Reading the source");
    const auto planningStart = std::chrono::steady_clock::now();
    auto planResult = planOnly(request, events);
    outcome.planningSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - planningStart).count();
    if (!planResult.has_value()) {
        outcome.finalState = JobState::Failed;
        outcome.message = planResult.error().message;
        if (!planResult.error().detail.empty()) {
            outcome.message += " (" + planResult.error().detail + ")";
        }
        setState(JobState::Failed, outcome.message);
        return outcome;
    }
    SplitPlan plan = planResult.value();
    outcome.plan = plan;
    outcome.warnings = plan.warnings;
    for (const std::string& warning : plan.warnings) {
        log("warning: " + warning);
    }
    if (!plan.planned) {
        outcome.finalState = JobState::Failed;
        outcome.message = "The split could not be planned: " + plan.failureExplanation;
        setState(JobState::Failed, outcome.message);
        return outcome;
    }

    setState(JobState::Planned, "Plan ready: " + std::to_string(plan.partCount()) + " part(s)");
    log(plan.describe());

    // Pre-flight: free space.
    {
        std::error_code error;
        const std::filesystem::space_info space = std::filesystem::space(request.outputDirectory, error);
        if (!error && plan.predictedTotalBytes > 0 &&
            static_cast<ByteCount>(space.available) < plan.predictedTotalBytes) {
            outcome.warnings.push_back(
                "The output drive has less free space (" + formatBytesShort(static_cast<ByteCount>(space.available)) +
                ") than the plan needs (" + formatBytesShort(plan.predictedTotalBytes) + ").");
            log("warning: " + outcome.warnings.back());
        }
    }

    OutputCommitter committer(request.outputDirectory, request.jobTag, request.naming.overwriteExisting);
    Status beginStatus = committer.begin();
    if (!beginStatus.ok()) {
        outcome.finalState = JobState::Failed;
        outcome.message = beginStatus.error.message;
        setState(JobState::Failed, outcome.message);
        return outcome;
    }
    log("working folder: " + committer.jobDirectory().string());

    // Warnings that the plan itself carries: source-duration estimates, packets
    // without timestamps, and so on.
    if (!plan.sourceDurationExact && plan.sourceDurationKnown) {
        outcome.warnings.push_back("the source duration is an estimate; every part is verified individually");
    }

    SizeModel sizeModel = plan.sizeModel;
    VerificationEngine verifier(prober_, runner_, ffmpegPath_, request.verification);
    std::vector<SegmentMeasurement> measurements;
    measurements.reserve(plan.segments.size());

    const auto executionStart = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < plan.segments.size(); ++index) {
        // Pause takes effect between parts; the part that is being written is
        // always finished so that no partial file is ever produced.
        while (pauseRequested() && !canceled()) {
            setState(JobState::Paused, "Paused after part " + std::to_string(index));
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (canceled()) {
            committer.cleanUp();
            outcome.canceled = true;
            outcome.message = "Cancelled. " + std::to_string(outcome.publishedFiles.size()) +
                              " part(s) had already been published and were verified.";
            setState(JobState::Cancelled, outcome.message);
            return outcome;
        }

        SegmentPlan segment = plan.segments[index];
        setState(JobState::Executing,
                 "Writing part " + std::to_string(segment.index) + " of " + std::to_string(segment.total));

        // Resolve the file name against what is already on disk.  A collision is
        // resolved by renaming, never by overwriting.
        std::filesystem::path finalPath = std::filesystem::path(request.outputDirectory) / segment.outputFileName;
        if (!request.naming.overwriteExisting && std::filesystem::exists(finalPath)) {
            for (int attempt = 2; attempt <= 999; ++attempt) {
                std::filesystem::path stem = finalPath.stem();
                std::filesystem::path extension = finalPath.extension();
                std::filesystem::path candidate =
                    finalPath.parent_path() / (stem.string() + " (" + std::to_string(attempt) + ")" + extension.string());
                if (!std::filesystem::exists(candidate)) {
                    finalPath = candidate;
                    segment.outputFileName = candidate.filename().string();
                    log("the planned name was taken; part " + std::to_string(segment.index) + " is written as " +
                        candidate.filename().string());
                    break;
                }
            }
        }

        int attempt = 0;
        bool partDone = false;
        while (!partDone) {
            ++attempt;
            const std::filesystem::path temporaryPath = committer.temporaryPathFor(segment.outputFileName);
            auto rendered = renderPart(plan, segment, temporaryPath, request, events);
            if (!rendered.has_value()) {
                committer.discard(temporaryPath);
                if (rendered.error().code == ErrorCode::ProcessCanceled) {
                    committer.cleanUp();
                    outcome.canceled = true;
                    outcome.message = "Cancelled while writing part " + std::to_string(segment.index) + ".";
                    setState(JobState::Cancelled, outcome.message);
                    return outcome;
                }
                committer.cleanUp();
                outcome.finalState = JobState::Failed;
                outcome.message = "Part " + std::to_string(segment.index) + " could not be written: " +
                                  rendered.error().message;
                log(rendered.error().toString());
                setState(JobState::Failed, outcome.message);
                return outcome;
            }
            SegmentOutcome partOutcome = rendered.value().outcome;

            setState(JobState::Verifying, "Verifying part " + std::to_string(segment.index));
            const auto verificationStart = std::chrono::steady_clock::now();
            const std::size_t measurementsBefore = measurements.size();
            auto verification = verifier.verifyPart(plan, segment, temporaryPath, &measurements, events.shouldCancel);
            outcome.verificationSeconds +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - verificationStart).count();
            if (!verification.has_value()) {
                committer.discard(temporaryPath);
                committer.cleanUp();
                outcome.finalState = JobState::Failed;
                outcome.message = verification.error().message;
                setState(JobState::Failed, outcome.message);
                return outcome;
            }
            for (const auto& finding : verification.value().findings) {
                if (finding.severity != FindingSeverity::Info) {
                    partOutcome.findings.push_back(finding.message);
                }
            }

            // The duration the user is shown and the job record stores is the one
            // the verifier *measured* on the written file, never the prediction
            // from the plan.
            if (measurements.size() > measurementsBefore && measurements.back().durationKnown) {
                partOutcome.duration = measurements.back().duration;
            }

            // Learn the container overhead from what really landed on disk.  The
            // "payload" that the model compares against is the payload the planner
            // booked, so any prediction error is absorbed by the model.
            SizeObservation observation;
            observation.predictedPayloadBytes = segment.predictedPayloadBytes;
            observation.actualPayloadBytes = segment.predictedPayloadBytes;
            observation.actualFileBytes = partOutcome.fileSizeBytes;
            observation.duration = measurements.size() > measurementsBefore
                                       ? measurements.back().duration
                                       : segment.predictedDuration;
            observation.packetCount = 0;
            observation.keyframeCount = 0;

            if (verification.value().passed) {
                // Optional closeness pass: write the largest still-safe part once
                // more, verify it, and keep it only if it is both bigger and valid.
                if (request.closenessPasses > 0 && plan.constraint.hasSizeLimit()) {
                    const ByteCount limit = plan.constraint.size->maxBytes;
                    const ByteCount slack = limit > partOutcome.fileSizeBytes ? limit - partOutcome.fileSizeBytes : 0;
                    const bool worthTrying = slack > kClosenessThresholdBytes &&
                                             static_cast<double>(slack) / static_cast<double>(limit) >
                                                 kClosenessThreshold;
                    if (worthTrying) {
                        sizeModel.observe(observation);
                        SplitPlanner closerPlanner(source_.container, source_.packets, plan.constraint,
                                                   request.naming, sizeModel, request.plannerSettings);
                        auto bigger = closerPlanner.planOnePart(plan, segment.startSeconds, segment.index,
                                                                segment.total, request.outputDirectory);
                        if (bigger.has_value() && compareRational(bigger.value().endSeconds, segment.endSeconds) > 0) {
                            const std::filesystem::path retryPath =
                                committer.temporaryPathFor(segment.outputFileName + ".closer");
                            auto renderedRetry = renderPart(plan, bigger.value(), retryPath, request, events);
                            if (renderedRetry.has_value()) {
                                const std::size_t beforeRetry = measurements.size();
                                auto retryVerification =
                                    verifier.verifyPart(plan, bigger.value(), retryPath, &measurements,
                                                        events.shouldCancel);
                                if (retryVerification.has_value() && retryVerification.value().passed &&
                                    renderedRetry.value().outcome.fileSizeBytes > partOutcome.fileSizeBytes) {
                                    log("part " + std::to_string(segment.index) + ": the first result used " +
                                        formatBytesShort(slack) + " less than the limit; the larger boundary " +
                                        formatDuration(bigger.value().endSeconds) + " was written and verified");
                                    committer.discard(temporaryPath);
                                    partOutcome = renderedRetry.value().outcome;
                                    segment = bigger.value();
                                    outcome.partsRewrittenForCloseness++;
                                    if (measurements.size() > beforeRetry) {
                                        // The retry is the file that is kept, so its
                                        // measurement replaces the discarded part's.
                                        const SegmentMeasurement retryMeasurement = measurements.back();
                                        measurements.erase(measurements.begin() +
                                                           static_cast<std::ptrdiff_t>(measurementsBefore));
                                        if (retryMeasurement.durationKnown) {
                                            partOutcome.duration = retryMeasurement.duration;
                                        }
                                        SizeObservation retryObservation = observation;
                                        retryObservation.actualFileBytes = partOutcome.fileSizeBytes;
                                        retryObservation.duration = retryMeasurement.durationKnown
                                                                        ? retryMeasurement.duration
                                                                        : bigger.value().predictedDuration;
                                        sizeModel.observe(retryObservation);
                                    }
                                } else {
                                    committer.discard(retryPath);
                                    if (measurements.size() > beforeRetry) {
                                        measurements.erase(measurements.begin() +
                                                           static_cast<std::ptrdiff_t>(beforeRetry));
                                    }
                                }
                            } else {
                                committer.discard(retryPath);
                            }
                        } else {
                            // Keep the calibration from the verified part.
                        }
                    }
                }

                // Publish.
                setState(JobState::Publishing, "Publishing part " + std::to_string(segment.index));
                const Status flushStatus = committer.flushToDisk(temporaryPath);
                if (!flushStatus.ok()) {
                    log(flushStatus.error.toString());
                }
                const Status commitStatus = committer.commit(temporaryPath, finalPath);
                if (!commitStatus.ok()) {
                    committer.discard(temporaryPath);
                    committer.cleanUp();
                    outcome.finalState = JobState::Failed;
                    outcome.message = commitStatus.error.message;
                    setState(JobState::Failed, outcome.message);
                    return outcome;
                }

                partOutcome.path = finalPath.string();
                partOutcome.verified = true;
                partOutcome.committed = true;
                {
                    std::error_code sizeError;
                    const std::uintmax_t publishedSize = std::filesystem::file_size(finalPath, sizeError);
                    if (!sizeError) {
                        partOutcome.fileSizeBytes = static_cast<ByteCount>(publishedSize);
                    }
                }
                partOutcome.verificationSummary = verification.value().summary;
                outcome.parts.push_back(partOutcome);
                outcome.publishedFiles.push_back(finalPath);
                log("published " + finalPath.filename().string() + " (" +
                    formatBytesShort(partOutcome.fileSizeBytes) + ", " +
                    formatDuration(partOutcome.duration) + ")");
                if (events.onProgress) {
                    events.onProgress(segment.index, segment.total,
                                      static_cast<double>(index + 1) / static_cast<double>(plan.segments.size()),
                                      "published");
                }
                partDone = true;
                break;
            }

            // ---- the part failed verification -----------------------------
            std::string findings = verification.value().renderFindings();
            log("part " + std::to_string(segment.index) + " did not pass verification:\n" + findings);
            partOutcome.verificationSummary = verification.value().summary;
            partOutcome.wrote = true;
            sizeModel.observe(observation);
            committer.discard(temporaryPath);
            if (measurements.size() > measurementsBefore) {
                measurements.erase(measurements.begin() + static_cast<std::ptrdiff_t>(measurementsBefore));
            }

            if (attempt >= request.maximumAttemptsPerPart) {
                committer.cleanUp();
                outcome.finalState = JobState::Failed;
                outcome.message = "Part " + std::to_string(segment.index) + " did not fit the limits after " +
                                  std::to_string(attempt) + " attempts. No part that breaks a limit is published.";
                setState(JobState::Failed, outcome.message);
                return outcome;
            }

            // Correct the rest of the plan with what was measured.
            SplitPlanner planner(source_.container, source_.packets, plan.constraint, request.naming, sizeModel,
                                 request.plannerSettings);
            auto replanned = planner.replanRemaining(plan, index);
            if (!replanned.has_value() || !replanned.value().planned ||
                replanned.value().segments.size() <= index) {
                committer.cleanUp();
                outcome.finalState = JobState::Failed;
                outcome.message = "Part " + std::to_string(segment.index) +
                                  " cannot be made smaller without breaking the plan: " +
                                  (replanned.has_value() ? replanned.value().failureExplanation
                                                         : replanned.error().message);
                setState(JobState::Failed, outcome.message);
                return outcome;
            }
            const SegmentPlan& corrected = replanned.value().segments[index];
            if (compareRational(corrected.endSeconds, segment.endSeconds) >= 0) {
                committer.cleanUp();
                outcome.finalState = JobState::Failed;
                outcome.message = "Part " + std::to_string(segment.index) +
                                  " stays over the limit even at the previous keyframe. The job stopped instead of "
                                  "publishing a file that breaks the limit.";
                setState(JobState::Failed, outcome.message);
                return outcome;
            }
            log("part " + std::to_string(segment.index) + ": the next attempt ends at " +
                formatDuration(corrected.endSeconds) + " instead of " + formatDuration(segment.endSeconds));
            plan = replanned.value();
            segment = corrected;
            outcome.partsRewrittenAfterFailure++;
        }
    }

    outcome.executionSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - executionStart).count();

    // Sequence-level check: coverage, completeness and the sum of the durations.
    if (outcome.parts.size() == plan.segments.size()) {
        auto sequence = verifier.verifySequence(plan, measurements);
        if (sequence.has_value()) {
            log("sequence check: " + sequence.value().renderFindings());
            for (const auto& finding : sequence.value().findings) {
                if (finding.severity == FindingSeverity::Error) {
                    outcome.warnings.push_back(finding.message);
                }
            }
        }
    }

    committer.cleanUp();
    outcome.succeeded = true;

    std::string message = std::to_string(outcome.parts.size()) + " part(s) written and verified";
    if (plan.constraint.hasSizeLimit()) {
        message += ". Largest part " + formatBytesShort(outcome.largestPublishedBytes()) + " against a limit of " +
                   formatBytesShort(plan.constraint.size->maxBytes);
    }
    if (plan.constraint.hasDurationLimit()) {
        message += ". Longest part " + formatDuration(outcome.longestPublishedDuration()) + " against a limit of " +
                   formatDuration(plan.constraint.duration->maxSeconds);
    }
    if (outcome.partsRewrittenAfterFailure > 0) {
        message += ". " + std::to_string(outcome.partsRewrittenAfterFailure) +
                   " part(s) were written again after a measured correction";
    }
    if (outcome.partsRewrittenForCloseness > 0) {
        message += ". " + std::to_string(outcome.partsRewrittenForCloseness) +
                   " part(s) were written again to get closer to the limit";
    }
    outcome.message = message;
    setState(JobState::Completed, message);
    return outcome;
}

} // namespace monosect::core
