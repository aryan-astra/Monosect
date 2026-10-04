// MonoSect — headless harness.
//
// This is not the product.  It exists so that the core (probing, planning,
// execution, verification, job history) can be built and exercised without a
// window - on the Linux development machine and in CI - and so that the
// end-to-end tests in tests/harness/ can drive a real job with real media.  The
// product itself is the Windows GUI in src/ui.
//
// Everything in this file goes through the same engine the GUI uses.  It never
// re-implements planning, copying or verification; when the harness and the
// application disagree, that is a bug in one of them, not a shortcut.
#include "core/CapabilityMatrix.hpp"
#include "core/Constraints.hpp"
#include "core/Json.hpp"
#include "core/PacketIndex.hpp"
#include "core/ProbeParsing.hpp"
#include "core/SplitPlanner.hpp"
#include "engine/FFprobeService.hpp"
#include "engine/JobRunner.hpp"
#include "core/MergePlanner.hpp"
#include "engine/JobStore.hpp"
#include "engine/MergeRunner.hpp"
#include "engine/ProcessRunners.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <csignal>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace monosect::core;

namespace {

/// Set when the user interrupts the harness (Ctrl+C / SIGINT / SIGTERM) or the
/// process is asked to stop.  The handler only flips this flag: the engine polls
/// it between work items, terminates the running FFmpeg process and discards
/// everything that was not verified, which is the same path the GUI uses when the
/// user presses Cancel.
///
/// The mechanism is platform specific because the console model is: a Windows
/// console delivers CTRL_C_EVENT through SetConsoleCtrlHandler (there is no
/// SIGINT), while POSIX delivers a signal.  Both handlers do the same one thing -
/// flip the flag - and nothing else may be done inside a console-control handler.
volatile ::sig_atomic_t g_cancelRequested = 0;

#if defined(_WIN32)
BOOL WINAPI handleConsoleControl(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_cancelRequested = 1;
            return TRUE;
        default:
            return FALSE;
    }
}

void installCancelHandler() { ::SetConsoleCtrlHandler(handleConsoleControl, TRUE); }
#else
extern "C" void handleCancelSignal(int /*signal*/) {
    g_cancelRequested = 1;
}

void installCancelHandler() {
    std::signal(SIGINT, handleCancelSignal);
    std::signal(SIGTERM, handleCancelSignal);
}
#endif

struct Options {
    std::string command;
    std::string input;
    std::string outputDirectory;
    std::string pattern = "{name} - part {index:03}";
    std::optional<SizeLimit> sizeLimit;
    std::string sizeLimitText;
    std::optional<DurationLimit> durationLimit;
    std::string durationLimitText;
    std::optional<Rational> splitAt;
    std::optional<Rational> targetDuration;
    std::optional<int> maxParts;
    PreservationMode preservation = PreservationMode::KeyframeAligned;
    PrecisionPolicy precision = PrecisionPolicy::Balanced;
    std::string ffmpeg = "ffmpeg";
    std::string ffprobe = "ffprobe";
    std::string database;
    std::vector<std::string> positional;   // merge: output directory, then the inputs
    std::string mergeName;
    std::string mergeExtension;
    bool verifyDecode = false;
    bool verifyHash = false;
    bool json = false;
    bool quiet = false;
    bool overwrite = false;
    bool keepTemp = false;
    int closenessPasses = 1;
    int limit = 20;
};

void printUsage() {
    std::cout <<
        "MonoSect harness (not the product - see src/ui for the application)\n"
        "\n"
        "  monosect-cli probe <file> [--json]\n"
        "  monosect-cli plan  <file> [constraints] [--out <dir>]\n"
        "  monosect-cli split <file> [constraints] --out <dir> [--verify-decode] [--verify-hash]\n"
        "  monosect-cli merge <output-dir> <file> <file> [file...] [--name <file>]\n"
        "  monosect-cli jobs  [--db <file>] [--limit N]\n"
        "\n"
        "constraints:\n"
        "  --limit <size>        hard maximum size per part (KiB, MiB, GiB, kB, MB, GB, bytes)\n"
        "  --duration <time>     hard maximum duration per part (seconds, mm:ss, hh:mm:ss.mmm)\n"
        "  --target <time>       soft target duration per part\n"
        "  --split-at <time>     cut once at this position\n"
        "  --parts <n>           refuse a plan that needs more parts than this\n"
        "  --mode <mode>         copy | keyframe | smart | reencode\n"
        "  --precision <p>       fast | balanced | maximum\n"
        "  --pattern <pattern>   output name pattern\n"
        "  --out <dir>           output directory\n"
        "  --overwrite           allow replacing existing files\n"
        "  --closeness <n>       extra renders that approach a size limit (default 1)\n"
        "  --keep-temp           keep the temporary folder after a failure\n"
        "  --verify-decode       decode every part as an extra verification step\n"
        "  --verify-hash         compare the copied streams with the source bit by bit\n"
        "  --name <file>         merge: the name of the merged file\n"
        "  --ext <extension>     merge: the container extension (default: the first input's)\n"
        "  --db <file>           job history file (default: the user's data folder)\n"
        "  --ffmpeg <path>       ffmpeg to use\n"
        "  --ffprobe <path>      ffprobe to use\n"
        "  --json                machine-readable output\n"
        "  --quiet               only the summary\n";
}

[[nodiscard]] bool parseSizeLimit(const std::string& text, SizeLimit* out, std::string* error) {
    auto parsed = parseByteSize(text);
    if (!parsed.has_value()) {
        *error = parsed.error().message;
        return false;
    }
    out->maxBytes = parsed.value().bytes;
    return true;
}

[[nodiscard]] bool parseDurationLimit(const std::string& text, DurationLimit* out, std::string* error) {
    auto parsed = parseDuration(text);
    if (!parsed.has_value()) {
        *error = parsed.error().message;
        return false;
    }
    out->maxSeconds = parsed.value();
    return true;
}

[[nodiscard]] bool parseArguments(int argc, char** argv, Options* options, std::string* error) {
    if (argc < 2) {
        *error = "no command given";
        return false;
    }
    options->command = argv[1];
    int index = 2;
    if (options->command == "probe" || options->command == "plan" || options->command == "split") {
        if (index >= argc) {
            *error = "that command needs a file";
            return false;
        }
        options->input = argv[index++];
    } else if (options->command == "merge") {
        // Every non-option argument is positional: the first is the output
        // directory, the rest are the inputs, in the order they are joined.
        while (index < argc && (argv[index][0] != '-' || argv[index][1] == '\0')) {
            options->positional.emplace_back(argv[index++]);
        }
        if (options->positional.size() < 3) {
            *error = "merge needs an output directory and at least two files";
            return false;
        }
    }
    for (; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&](std::string* value) -> bool {
            if (index + 1 >= argc) {
                return false;
            }
            *value = argv[++index];
            return true;
        };
        std::string value;
        if (argument == "--limit") {
            if (!next(&value)) { *error = "--limit needs a value"; return false; }
            SizeLimit limit;
            if (!parseSizeLimit(value, &limit, error)) { return false; }
            options->sizeLimit = limit;
            options->sizeLimitText = value;
        } else if (argument == "--duration") {
            if (!next(&value)) { *error = "--duration needs a value"; return false; }
            DurationLimit limit;
            if (!parseDurationLimit(value, &limit, error)) { return false; }
            options->durationLimit = limit;
            options->durationLimitText = value;
        } else if (argument == "--target") {
            if (!next(&value)) { *error = "--target needs a value"; return false; }
            auto parsed = parseDuration(value);
            if (!parsed.has_value()) { *error = parsed.error().message; return false; }
            options->targetDuration = parsed.value();
        } else if (argument == "--split-at") {
            if (!next(&value)) { *error = "--split-at needs a value"; return false; }
            auto parsed = parseDuration(value);
            if (!parsed.has_value()) { *error = parsed.error().message; return false; }
            options->splitAt = parsed.value();
        } else if (argument == "--parts") {
            if (!next(&value)) { *error = "--parts needs a value"; return false; }
            options->maxParts = std::atoi(value.c_str());
        } else if (argument == "--mode") {
            if (!next(&value)) { *error = "--mode needs a value"; return false; }
            if (value == "copy") {
                options->preservation = PreservationMode::StreamCopy;
            } else if (value == "keyframe") {
                options->preservation = PreservationMode::KeyframeAligned;
            } else if (value == "smart") {
                options->preservation = PreservationMode::SmartCut;
            } else if (value == "reencode") {
                options->preservation = PreservationMode::FullReEncode;
            } else {
                *error = "--mode must be copy, keyframe, smart or reencode";
                return false;
            }
        } else if (argument == "--precision") {
            if (!next(&value)) { *error = "--precision needs a value"; return false; }
            if (value == "fast") {
                options->precision = PrecisionPolicy::Fast;
            } else if (value == "balanced") {
                options->precision = PrecisionPolicy::Balanced;
            } else if (value == "maximum") {
                options->precision = PrecisionPolicy::Maximum;
            } else {
                *error = "--precision must be fast, balanced or maximum";
                return false;
            }
        } else if (argument == "--pattern") {
            if (!next(&value)) { *error = "--pattern needs a value"; return false; }
            options->pattern = value;
        } else if (argument == "--out") {
            if (!next(&value)) { *error = "--out needs a value"; return false; }
            options->outputDirectory = value;
        } else if (argument == "--ext") {
            if (!next(&value)) { *error = "--ext needs a value"; return false; }
            options->mergeExtension = value;
        } else if (argument == "--name") {
            if (!next(&value)) { *error = "--name needs a value"; return false; }
            options->mergeName = value;
        } else if (argument == "--db") {
            if (!next(&value)) { *error = "--db needs a value"; return false; }
            options->database = value;
        } else if (argument == "--ffmpeg") {
            if (!next(&value)) { *error = "--ffmpeg needs a value"; return false; }
            options->ffmpeg = value;
        } else if (argument == "--ffprobe") {
            if (!next(&value)) { *error = "--ffprobe needs a value"; return false; }
            options->ffprobe = value;
        } else if (argument == "--closeness") {
            if (!next(&value)) { *error = "--closeness needs a value"; return false; }
            options->closenessPasses = std::atoi(value.c_str());
        } else if (argument == "--limit-jobs") {
            if (!next(&value)) { *error = "--limit-jobs needs a value"; return false; }
            options->limit = std::atoi(value.c_str());
        } else if (argument == "--verify-decode") {
            options->verifyDecode = true;
        } else if (argument == "--verify-hash") {
            options->verifyHash = true;
        } else if (argument == "--overwrite") {
            options->overwrite = true;
        } else if (argument == "--keep-temp") {
            options->keepTemp = true;
        } else if (argument == "--json") {
            options->json = true;
        } else if (argument == "--quiet") {
            options->quiet = true;
        } else if (argument == "--help" || argument == "-h") {
            printUsage();
            std::exit(0);
        } else {
            *error = "unknown option: " + argument;
            return false;
        }
    }
    const char* envFfmpeg = std::getenv("MONOSECT_FFMPEG");
    const char* envFfprobe = std::getenv("MONOSECT_FFPROBE");
    if (envFfmpeg != nullptr && options->ffmpeg == "ffmpeg") {
        options->ffmpeg = envFfmpeg;
    }
    if (envFfprobe != nullptr && options->ffprobe == "ffprobe") {
        options->ffprobe = envFfprobe;
    }
    return true;
}

[[nodiscard]] UserConstraint constraintFor(const Options& options) {
    UserConstraint constraint;
    constraint.size = options.sizeLimit;
    constraint.duration = options.durationLimit;
    constraint.preservation = options.preservation;
    constraint.precision = options.precision;
    constraint.splitAtSeconds = options.splitAt;
    constraint.targetPartSeconds = options.targetDuration;
    constraint.maxParts = options.maxParts;
    return constraint;
}

[[nodiscard]] std::string jsonString(std::string_view text) {
    return "\"" + jsonEscape(text) + "\"";
}

[[nodiscard]] std::string optionalIntJson(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : std::string("null");
}

[[nodiscard]] std::string optionalIntJson(const std::optional<int>& value) {
    return value.has_value() ? std::to_string(*value) : std::string("null");
}

[[nodiscard]] std::string optionalRationalJson(const std::optional<Rational>& value) {
    return value.has_value() ? jsonString(rationalToString(*value)) : std::string("null");
}

[[nodiscard]] std::string streamJson(const StreamModel& stream) {
    std::string out = "{";
    out += "\"index\":" + std::to_string(stream.index);
    out += ",\"kind\":" + jsonString(streamKindName(stream.kind));
    out += ",\"codec\":" + jsonString(stream.codecName);
    out += ",\"profile\":" + jsonString(stream.profile);
    out += ",\"level\":" + std::to_string(stream.level);
    out += ",\"timebase\":" + jsonString(rationalToString(stream.timebase));
    if (stream.kind == StreamKind::Video) {
        out += ",\"width\":" + std::to_string(stream.width);
        out += ",\"height\":" + std::to_string(stream.height);
        out += ",\"pixel_format\":" + jsonString(stream.pixelFormat);
        out += ",\"avg_frame_rate\":" + jsonString(rationalToString(stream.averageFrameRate));
        out += ",\"r_frame_rate\":" + jsonString(rationalToString(stream.baseFrameRate));
    }
    if (stream.kind == StreamKind::Audio) {
        out += ",\"sample_rate\":" + optionalIntJson(stream.sampleRate);
        out += ",\"channels\":" + optionalIntJson(stream.channels);
        out += ",\"channel_layout\":" + jsonString(stream.channelLayout);
    }
    out += ",\"start_time\":" + optionalRationalJson(stream.startTime);
    out += ",\"duration\":" + optionalRationalJson(stream.duration);
    out += ",\"bit_rate\":" + optionalIntJson(stream.bitRate);
    out += ",\"frame_count\":" + optionalIntJson(stream.frameCount);
    out += ",\"language\":" + jsonString(stream.language);
    out += ",\"title\":" + jsonString(stream.title);
    out += ",\"is_default\":" + std::string(stream.isDefault ? "true" : "false");
    out += ",\"is_forced\":" + std::string(stream.isForced ? "true" : "false");
    out += ",\"summary\":" + jsonString(stream.parameterSummary());
    out += "}";
    return out;
}

/// The probe JSON is a stable view of the model: what MonoSect knows, how sure
/// it is, and where the number came from.  Nothing is rounded away.
[[nodiscard]] std::string mediaModelJson(const MediaModel& model, const PacketIndex& index) {
    std::string out = "{";
    out += "\"path\":" + jsonString(model.path);
    out += ",\"file_size_bytes\":" + std::to_string(model.fileSize);
    out += ",\"format_names\":" + jsonString(model.formatNames);
    out += ",\"format_long_name\":" + jsonString(model.formatLongName);
    out += ",\"friendly_format\":" + jsonString(friendlyFormatName(model));
    out += ",\"start_time\":" + jsonString(rationalToString(model.startTime));
    out += ",\"duration\":" + (model.duration.has_value() ? jsonString(rationalToString(*model.duration)) : std::string("null"));
    out += ",\"duration_resolved\":{";
    out += "\"seconds\":" + optionalRationalJson(model.durationResolved.seconds);
    out += ",\"source\":" + jsonString(durationSourceName(model.durationResolved.source));
    out += ",\"reliable\":" + std::string(model.durationResolved.reliable ? "true" : "false");
    out += ",\"explanation\":" + jsonString(model.durationResolved.explanation);
    out += "}";
    out += ",\"is_still_image\":" + std::string(model.isStillImage ? "true" : "false");
    out += ",\"is_live_stream_like\":" + std::string(model.isLiveStreamLike ? "true" : "false");
    out += ",\"variable_frame_rate\":" + std::string(model.frameRate.variable ? "true" : "false");
    out += ",\"frame_rate_explanation\":" + jsonString(model.frameRate.explanation);
    out += ",\"streams\":[";
    for (std::size_t i = 0; i < model.streams.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += streamJson(model.streams[i]);
    }
    out += "]";
    out += ",\"chapters_count\":" + std::to_string(model.chapters.size());
    if (!index.empty()) {
        out += ",\"packets\":{";
        out += "\"total\":" + std::to_string(index.totalPacketCount());
        out += ",\"payload_bytes\":" + std::to_string(index.totalPayloadBytes());
        out += ",\"untimed\":" + std::to_string(index.untimedPacketCount());
        out += ",\"per_stream\":[";
        for (std::size_t i = 0; i < index.tracks().size(); ++i) {
            const StreamPacketTrack& track = index.tracks()[i];
            if (i > 0) {
                out += ",";
            }
            out += "{\"index\":" + std::to_string(track.streamIndex());
            out += ",\"packets\":" + std::to_string(track.size());
            out += ",\"keyframes\":" + std::to_string(track.keyframeCount());
            out += ",\"payload_bytes\":" + std::to_string(track.totalPayloadBytes());
            out += ",\"reorders\":" + std::string(track.hasReordering() ? "true" : "false");
            out += ",\"first_pts\":" +
                   (track.firstPresentationTime().has_value() ? jsonString(rationalToString(*track.firstPresentationTime()))
                                                              : std::string("null"));
            out += ",\"last_end\":" +
                   (track.lastEndTime().has_value() ? jsonString(rationalToString(*track.lastEndTime())) : std::string("null"));
            out += "}";
        }
        out += "]}";
    }
    out += ",\"warnings\":[";
    for (std::size_t i = 0; i < model.warnings.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += jsonString(model.warnings[i]);
    }
    out += "]";
    out += "}";
    return out;
}

[[nodiscard]] std::string streamLine(const StreamModel& stream) {
    std::string line = "  #" + std::to_string(stream.index) + "  " + std::string(streamKindName(stream.kind)) + "  " +
                       stream.parameterSummary();
    if (stream.bitRate.has_value()) {
        line += "  " + formatBytesShort(*stream.bitRate / 8) + "/s";
    }
    if (!stream.language.empty()) {
        line += "  [" + stream.language + "]";
    }
    if (stream.isDefault) {
        line += "  (default)";
    }
    if (stream.isForced) {
        line += "  (forced)";
    }
    return line;
}

int commandProbe(const Options& options) {
    IProcessRunner& runner = defaultProcessRunner();
    FFprobeService prober(runner, options.ffprobe);
    auto snapshot = prober.probeAll(options.input);
    if (!snapshot.has_value()) {
        std::cerr << "error: " << snapshot.error().toString() << "\n";
        return 1;
    }
    const MediaModel& model = snapshot.value().container;
    const PacketIndex& index = snapshot.value().packets;
    if (options.json) {
        std::cout << mediaModelJson(model, index) << "\n";
        return 0;
    }
    std::cout << model.path << "\n";
    const std::string containerName =
        !model.formatLongName.empty() ? model.formatLongName : friendlyFormatName(model);
    std::cout << "  container: " << containerName << ", " << formatBytesExact(model.fileSize);
    if (model.durationResolved.known()) {
        std::cout << ", " << formatDuration(*model.durationResolved.seconds);
        std::cout << " (" << durationSourceName(model.durationResolved.source)
                  << (model.durationResolved.reliable ? ", reliable" : ", NOT reliable") << ")";
    } else {
        std::cout << ", duration unknown";
    }
    std::cout << "\n";
    std::cout << "  " << model.durationResolved.explanation << "\n";
    for (const StreamModel& stream : model.streams) {
        std::cout << streamLine(stream) << "\n";
    }
    const StreamModel* videoStream = model.videoStream();
    const StreamPacketTrack* video = videoStream != nullptr ? index.track(videoStream->index) : nullptr;
    if (video != nullptr) {
        const auto keyframes = video->keyframeTimes();
        std::cout << "  video packets: " << video->size() << ", keyframes: " << keyframes.size();
        if (!keyframes.empty()) {
            std::cout << ", first at " << formatDuration(keyframes.front());
            if (keyframes.size() > 1) {
                std::cout << ", last at " << formatDuration(keyframes.back());
            }
        }
        std::cout << "\n";
        if (keyframes.size() >= 2 && keyframes.size() <= 12 && !options.quiet) {
            std::cout << "  keyframe positions:";
            for (const Rational& keyframe : keyframes) {
                std::cout << " " << formatDuration(keyframe);
            }
            std::cout << "\n";
        }
        if (video->hasReordering()) {
            std::cout << "  the stream reorders frames (dts differs from pts), so a part must start at a "
                         "keyframe to be decodable on its own\n";
        }
        if (video->untimedPacketCount() > 0) {
            std::cout << "  warning: " << video->untimedPacketCount() << " packet(s) carry no usable timestamp\n";
        }
    }
    for (const std::string& warning : model.warnings) {
        std::cout << "  warning: " << warning << "\n";
    }
    return 0;
}

int commandPlan(const Options& options) {
    IProcessRunner& runner = defaultProcessRunner();
    FFprobeService prober(runner, options.ffprobe);
    auto snapshot = prober.probeAll(options.input);
    if (!snapshot.has_value()) {
        std::cerr << "error: " << snapshot.error().toString() << "\n";
        return 1;
    }
    const UserConstraint constraint = constraintFor(options);
    const ConstraintReport report = validateConstraint(constraint, snapshot.value().container);
    for (const ConstraintIssue& issue : report.issues) {
        std::cerr << (issue.severity == ConstraintIssue::Severity::Error ? "error: " : "warning: ") << issue.message;
        if (!issue.remedy.empty()) {
            std::cerr << " " << issue.remedy;
        }
        std::cerr << "\n";
    }
    if (!report.valid) {
        return 2;
    }
    SizeModel sizeModel;
    sizeModel.seedFromSource(snapshot.value().container.formatNames, snapshot.value().container.fileSize,
                             snapshot.value().packets.totalPayloadBytes());
    NamingPolicy naming;
    naming.pattern = options.pattern;
    SplitPlanner planner(snapshot.value().container, snapshot.value().packets, constraint, naming, sizeModel);
    auto plan = planner.plan(options.outputDirectory.empty() ? "." : options.outputDirectory);
    if (!plan.has_value()) {
        std::cerr << "error: " << plan.error().toString() << "\n";
        return 1;
    }
    if (!plan.value().planned) {
        std::cerr << "This split cannot be planned: " << plan.value().failureExplanation << "\n";
        return 2;
    }
    if (options.json) {
        std::cout << plan.value().toJson() << "\n";
    } else {
        std::cout << plan.value().describe();
    }
    return 0;
}

int commandSplit(const Options& options) {
    if (options.outputDirectory.empty()) {
        std::cerr << "error: split needs --out <directory>\n";
        return 1;
    }
    IProcessRunner& runner = defaultProcessRunner();
    FFprobeService prober(runner, options.ffprobe);
    JobRunner jobRunner(prober, runner, options.ffmpeg);

    const auto ffprobeVersion = prober.version();

    JobRequest request;
    request.sourcePath = options.input;
    request.outputDirectory = options.outputDirectory;
    request.constraint = constraintFor(options);
    request.naming.pattern = options.pattern;
    request.naming.overwriteExisting = options.overwrite;
    request.verification.decodeCheck = options.verifyDecode;
    request.verification.contentHashCheck = options.verifyHash;
    request.closenessPasses = options.closenessPasses;
    request.keepTemporaryFilesOnFailure = options.keepTemp;
    request.databasePath = options.database;
    request.recordHistory = true;
    request.jobTag = std::filesystem::path(options.input).stem().string();
    request.appVersion = "1.2.0";
    request.ffprobeVersion = ffprobeVersion.has_value() ? ffprobeVersion.value() : std::string();

    JobEvents events;
    const bool quiet = options.quiet;
    events.onState = [&](JobState state, const std::string& message) {
        if (!quiet) {
            std::cout << "[" << jobStateName(state) << "] " << message << "\n";
        }
    };
    events.onLog = [&](const std::string& message) {
        if (!quiet) {
            std::cout << "  " << message << "\n";
        }
    };
    // Ctrl+C asks the job to stop; the engine terminates FFmpeg, discards the
    // unfinished part and reports a cancelled job (exit code 3).
    events.shouldCancel = []() { return g_cancelRequested != 0; };
    events.onPlanReady = [&](const SplitPlan& plan) {
        if (!quiet && !options.json) {
            std::cout << plan.describe();
        }
    };
    events.onProgress = [&](int partIndex, int partCount, double fraction, const std::string& stage) {
        if (quiet || options.json) {
            return;
        }
        std::cout << "\r  part " << partIndex << "/" << partCount << "  " << stage << "  "
                  << static_cast<int>(fraction * 100.0) << "%   " << std::flush;
        if (fraction >= 1.0) {
            std::cout << "\n";
        }
    };

    auto outcome = jobRunner.run(request, events);
    if (!outcome.has_value()) {
        std::cerr << "\nerror: " << outcome.error().toString() << "\n";
        return 1;
    }
    const JobOutcome& result = outcome.value();
    if (!options.json) {
        std::cout << "\n" << result.summary() << "\n";
        for (const SegmentOutcome& part : result.parts) {
            std::cout << "  part " << part.index << ": " << part.path << "  " << formatBytesExact(part.fileSizeBytes)
                      << "  " << (part.duration.isPositive() ? formatDuration(part.duration) : std::string("unknown"))
                      << "  " << segmentMethodName(part.method) << "  "
                      << (part.verified ? "verified" : "NOT VERIFIED");
            if (!part.verificationSummary.empty()) {
                std::cout << "  " << part.verificationSummary;
            }
            std::cout << "\n";
        }
        for (const std::string& warning : result.warnings) {
            std::cout << "  warning: " << warning << "\n";
        }
    } else {
        std::cout << result.summary() << "\n";
    }
    if (!result.succeeded) {
        return result.canceled ? 3 : 1;
    }
    return 0;
}

int commandMerge(const Options& options) {
    IProcessRunner& runner = defaultProcessRunner();
    FFprobeService prober(runner, options.ffprobe);
    MergeRunner mergeRunner(prober, runner, options.ffmpeg);

    const std::string outputDirectory = options.positional.front();
    const std::vector<std::string> inputs(options.positional.begin() + 1, options.positional.end());

    // A merge by copying has to stay in the container of its inputs, so the
    // extension defaults to the first input's own extension.
    std::string extension = options.mergeExtension;
    if (extension.empty()) {
        extension = std::filesystem::path(inputs.front()).extension().string();
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(0, 1);
        }
        if (extension.empty()) {
            extension = "mkv";
        }
    }

    MergeJobRequest request;
    request.inputs = inputs;
    request.outputDirectory = outputDirectory;
    request.outputFileName = options.mergeName;
    if (request.outputFileName.empty()) {
        const std::string stem = std::filesystem::path(inputs.front()).stem().string();
        request.outputFileName = sanitizeFileName(stem + " merged") + "." + extension;
    }
    const std::filesystem::path finalPath = std::filesystem::path(outputDirectory) / request.outputFileName;
    std::error_code existsError;
    if (std::filesystem::exists(finalPath, existsError) && !options.overwrite) {
        std::cerr << "error: " << finalPath.string()
                  << " already exists; pass --overwrite to replace it\n";
        return 1;
    }
    request.verification.decodeCheck = options.verifyDecode;
    request.verification.contentHashCheck = options.verifyHash;
    request.overwriteExisting = options.overwrite;
    request.keepTemporaryFilesOnFailure = options.keepTemp;
    request.databasePath = options.database;
    request.recordHistory = true;
    request.appVersion = "1.2.0";
    if (const auto version = prober.version(); version.has_value()) {
        request.ffprobeVersion = version.value();
    }

    JobEvents events;
    const bool quiet = options.quiet;
    events.onState = [&](JobState state, const std::string& message) {
        if (!quiet) {
            std::cout << "[" << jobStateName(state) << "] " << message << "\n";
        }
    };
    events.onLog = [&](const std::string& message) {
        if (!quiet) {
            std::cout << "  " << message << "\n";
        }
    };
    events.shouldCancel = []() { return g_cancelRequested != 0; };

    auto outcome = mergeRunner.run(request, events);
    if (!outcome.has_value()) {
        std::cerr << "error: " << outcome.error().toString() << "\n";
        // The same exit codes as the split command: 2 means "this cannot be
        // planned", which is what an incompatible set of inputs is.
        return outcome.error().code == ErrorCode::PlanFailed ? 2 : 1;
    }
    if (!options.json) {
        std::cout << outcome.value().summary() << "\n";
        for (const std::string& warning : outcome.value().warnings) {
            std::cout << "  warning: " << warning << "\n";
        }
    } else {
        std::cout << outcome.value().plan.toJson() << "\n";
    }
    if (!outcome.value().succeeded) {
        return outcome.value().canceled ? 3 : 1;
    }
    return 0;
}

int commandJobs(const Options& options) {
    JobStore store = options.database.empty() ? JobStore::defaultStore() : JobStore(options.database);
    auto records = store.loadRecent(options.limit);
    if (!records.has_value()) {
        std::cerr << "error: " << records.error().toString() << "\n";
        return 1;
    }
    std::cout << store.path().string() << ": " << records.value().size() << " record(s)";
    if (store.damagedRecordCount() > 0) {
        std::cout << ", " << store.damagedRecordCount() << " damaged line(s) skipped";
    }
    std::cout << "\n";
    for (const JobRecord& record : records.value()) {
        std::cout << "  " << record.finishedAt << "  " << record.jobId << "  " << record.finalState << "  "
                  << formatBytesShort(record.sourceBytes) << "  " << record.parts.size() << " part(s)  "
                  << record.outputDirectory << "\n";
        if (!record.message.empty()) {
            std::cout << "      " << record.message << "\n";
        }
        if (!record.constraintDescription.empty()) {
            std::cout << "      " << record.constraintDescription << "\n";
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    installCancelHandler();
    Options options;
    std::string error;
    if (!parseArguments(argc, argv, &options, &error)) {
        std::cerr << "error: " << error << "\n\n";
        printUsage();
        return 1;
    }
    if (options.command == "probe") {
        return commandProbe(options);
    }
    if (options.command == "plan") {
        return commandPlan(options);
    }
    if (options.command == "split") {
        return commandSplit(options);
    }
    if (options.command == "merge") {
        return commandMerge(options);
    }
    if (options.command == "jobs") {
        return commandJobs(options);
    }
    std::cerr << "error: unknown command: " << options.command << "\n\n";
    printUsage();
    return 1;
}
