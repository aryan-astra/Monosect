#include "engine/SplitEngine.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

namespace vsm {

namespace {

double clamp01(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

bool isCanceled(const SplitRequest& request) {
    return request.shouldCancel && request.shouldCancel();
}

bool waitIfPaused(const SplitRequest& request) {
    if (!request.shouldPause) {
        return !isCanceled(request);
    }
    while (request.shouldPause()) {
        if (isCanceled(request)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !isCanceled(request);
}

std::optional<double> parseDouble(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    double value = 0.0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec == std::errc() && ptr == end) {
        return value;
    }
    return std::nullopt;
}

std::optional<double> parseTimecodeSeconds(const std::string& value) {
    int hours = 0;
    int minutes = 0;
    double seconds = 0.0;
    char colon1 = '\0';
    char colon2 = '\0';

    std::istringstream stream(value);
    stream >> hours >> colon1 >> minutes >> colon2 >> seconds;
    if (!stream.fail() && colon1 == ':' && colon2 == ':') {
        return static_cast<double>(hours) * 3600.0 + static_cast<double>(minutes) * 60.0 + seconds;
    }
    return std::nullopt;
}

std::optional<double> parseProgressTimeSeconds(const std::string& line) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
        return std::nullopt;
    }

    const std::string key = line.substr(0, pos);
    const std::string value = line.substr(pos + 1);
    if (key == "out_time_us" || key == "out_time_ms") {
        if (const auto parsed = parseDouble(value); parsed.has_value()) {
            const double raw = *parsed;
            if (raw > 10000000.0) {
                return raw / 1000000.0;
            }
            return raw / 1000.0;
        }
        return std::nullopt;
    }
    if (key == "out_time") {
        return parseTimecodeSeconds(value);
    }
    return std::nullopt;
}

void replaceAll(std::string& source, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = source.find(from, pos)) != std::string::npos) {
        source.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string formatOutputName(const SplitRequest& request,
                             const std::string& stem,
                             int partIndex,
                             const std::string& ext) {
    std::string pattern = request.outputPattern.value_or("{stem}_part{n:02d}{ext}");
    replaceAll(pattern, "{stem}", stem);
    replaceAll(pattern, "{ext}", ext);

    std::ostringstream padded;
    padded << std::setw(2) << std::setfill('0') << partIndex;
    replaceAll(pattern, "{n:02d}", padded.str());
    replaceAll(pattern, "{n}", std::to_string(partIndex));
    return pattern;
}

void appendHwAccelArgs(std::vector<std::string>& args, const SplitRequest& request) {
    if (!request.useHardwareAcceleration || !request.codecOverride.has_value() || *request.codecOverride == "copy") {
        return;
    }
    const std::string& codec = *request.codecOverride;
    if (codec.find("nvenc") != std::string::npos) {
        args.push_back("-hwaccel");
        args.push_back("cuda");
        args.push_back("-hwaccel_output_format");
        args.push_back("cuda");
    } else if (codec.find("qsv") != std::string::npos) {
        args.push_back("-hwaccel");
        args.push_back("qsv");
    } else if (codec.find("amf") != std::string::npos) {
        args.push_back("-hwaccel");
        args.push_back("d3d11va");
    }
}

bool usesReencodePath(const SplitRequest& request) {
    return request.codecOverride.has_value() && *request.codecOverride != "copy";
}

void appendCodecArgs(std::vector<std::string>& args, const SplitRequest& request) {
    if (usesReencodePath(request)) {
        const std::string& codec = *request.codecOverride;
        args.push_back("-c:v");
        args.push_back(codec);
        args.push_back("-c:a");
        args.push_back("copy");
        args.push_back("-c:s");
        args.push_back("copy");

        if (codec.find("nvenc") != std::string::npos || codec.find("amf") != std::string::npos) {
            args.push_back("-preset");
            args.push_back("p4");
            args.push_back("-cq");
            args.push_back("23");
        } else if (codec.find("qsv") != std::string::npos) {
            args.push_back("-preset");
            args.push_back("medium");
            args.push_back("-global_quality");
            args.push_back("23");
        }
    } else {
        args.push_back("-c");
        args.push_back("copy");
    }
}

void appendThreadArgs(std::vector<std::string>& args, const SplitRequest& request) {
    if (!usesReencodePath(request)) {
        return;
    }
    if (request.desiredThreads.has_value() && *request.desiredThreads > 0) {
        const std::string threadCount = std::to_string(*request.desiredThreads);
        args.push_back("-threads");
        args.push_back(threadCount);
        args.push_back("-filter_threads");
        args.push_back(threadCount);
        args.push_back("-filter_complex_threads");
        args.push_back(threadCount);
        return;
    }
    args.push_back("-threads");
    args.push_back("0");
}

} // namespace

SplitEngine::SplitEngine() = default;

SplitEngine::SplitEngine(FFmpegProcess process)
    : process_(std::move(process)),
      gopAnalyzer_(process_),
      mediaProbe_(process_) {}

SplitResult SplitEngine::split(const SplitRequest& request) const {
    if (!isSafePath(request.inputPath) || !isSafePath(request.outputDirectory)) {
        return { false, "Unsafe path detected", {} };
    }

    std::error_code ec;
    if (!std::filesystem::exists(request.inputPath, ec) || ec) {
        return { false, "Input file does not exist", {} };
    }
    if (!std::filesystem::is_regular_file(request.inputPath, ec) || ec) {
        return { false, "Input path is not a regular file", {} };
    }

    if (!waitIfPaused(request)) {
        return { false, "Canceled", {} };
    }

    if (request.splitMode == SplitMode::ExactTime && request.splitTimeSeconds.has_value()) {
        return splitByTime(request, *request.splitTimeSeconds);
    }

    if (request.splitMode == SplitMode::HalfByTime) {
        const auto info = mediaProbe_.probe(request.inputPath);
        return splitByTime(request, info.durationSeconds / 2.0);
    }

    if ((request.splitMode == SplitMode::ExactSize || request.splitMode == SplitMode::HalfBySize || request.splitMode == SplitMode::TorrentPreset)
        && request.splitSizeBytes.has_value()) {
        return splitBySize(request, *request.splitSizeBytes);
    }

    if (request.splitMode == SplitMode::EqualParts && request.splitPartCount.has_value()) {
        return splitByEqualParts(request, *request.splitPartCount);
    }

    return { false, "Invalid split request", {} };
}

SplitResult SplitEngine::splitByTime(const SplitRequest& request,
                                     double splitTime,
                                     std::optional<MediaInfo> preProbed) const {
    if (!waitIfPaused(request)) {
        return { false, "Canceled", {} };
    }
    if (splitTime < 0.0 || !std::isfinite(splitTime)) {
        return { false, "Invalid split time", {} };
    }

    const MediaInfo info = preProbed.has_value() ? *preProbed : mediaProbe_.probe(request.inputPath);
    if (info.durationSeconds <= 0.0) {
        return { false, "Unable to determine media duration", {} };
    }

    if (splitTime >= info.durationSeconds) {
        splitTime = std::max(0.0, info.durationSeconds - 0.001);
    }

    const auto analysis = gopAnalyzer_.analyze(request.inputPath, splitTime, 45.0);
    const auto nearest = gopAnalyzer_.nearestKeyframe(analysis, splitTime);
    if (!nearest.has_value()) {
        return { false, "No keyframe found", {} };
    }

    std::filesystem::create_directories(request.outputDirectory);

    const auto stem = request.inputPath.stem().string();
    const auto ext = request.inputPath.extension().string();
    const auto first = request.outputDirectory / formatOutputName(request, stem, 1, ext);
    const auto second = request.outputDirectory / formatOutputName(request, stem, 2, ext);

    const std::string splitPoint = std::to_string(nearest->timeSeconds);
    const double firstDurationSeconds = std::max(0.0, std::min(nearest->timeSeconds, info.durationSeconds));
    const double secondDurationSeconds = std::max(0.0, info.durationSeconds - firstDurationSeconds);
    const double totalDurationSeconds = std::max(0.001, info.durationSeconds);
    const double firstWeight = clamp01(firstDurationSeconds / totalDurationSeconds);

    if (request.onProgress) {
        request.onProgress(0.0, formatSplitStatus("Preparing first segment", 0.0));
    }
    if (!waitIfPaused(request)) {
        return { false, "Canceled", {} };
    }

    std::vector<std::string> firstArgs = {
        "-hide_banner", "-nostats", "-progress", "pipe:2", "-y",
    };
    appendHwAccelArgs(firstArgs, request);
    firstArgs.push_back("-i");
    firstArgs.push_back(request.inputPath.string());
    firstArgs.insert(firstArgs.end(), {
        "-to", splitPoint,
        "-map", "0"
    });
    appendCodecArgs(firstArgs, request);
    firstArgs.push_back("-avoid_negative_ts");
    firstArgs.push_back("make_zero");
    appendThreadArgs(firstArgs, request);
    firstArgs.push_back(first.string());

    const auto r1 = process_.runFFmpeg(
        firstArgs,
        [&](const std::string& line) {
            if (!request.onProgress) {
                return;
            }
            if (const auto seconds = parseProgressTimeSeconds(line); seconds.has_value()) {
                const double partProgress = firstDurationSeconds > 0.0 ? clamp01(*seconds / firstDurationSeconds) : 1.0;
                const double overall = firstWeight * partProgress;
                request.onProgress(overall, formatSplitStatus("Splitting part 1/2", overall));
            }
        },
        [&request]() {
            return request.shouldCancel && request.shouldCancel();
        });
    if (!r1.ok) {
        return { false, r1.stdErr, {} };
    }
    if (request.onProgress) {
        request.onProgress(firstWeight, formatSplitStatus("Part 1/2 complete", firstWeight));
    }

    if (!waitIfPaused(request)) {
        return { false, "Canceled", {} };
    }
    std::vector<std::string> secondArgs = {
        "-hide_banner", "-nostats", "-progress", "pipe:2", "-y"
    };
    if (request.fastSeekEnabled) {
        secondArgs.push_back("-ss");
        secondArgs.push_back(splitPoint);
        appendHwAccelArgs(secondArgs, request);
        secondArgs.push_back("-i");
        secondArgs.push_back(request.inputPath.string());
    } else {
        appendHwAccelArgs(secondArgs, request);
        secondArgs.push_back("-i");
        secondArgs.push_back(request.inputPath.string());
        secondArgs.push_back("-ss");
        secondArgs.push_back(splitPoint);
    }
    secondArgs.push_back("-map");
    secondArgs.push_back("0");
    appendCodecArgs(secondArgs, request);
    secondArgs.push_back("-avoid_negative_ts");
    secondArgs.push_back("make_zero");
    appendThreadArgs(secondArgs, request);
    secondArgs.push_back(second.string());

    const auto r2 = process_.runFFmpeg(
        secondArgs,
        [&](const std::string& line) {
            if (!request.onProgress) {
                return;
            }
            if (const auto seconds = parseProgressTimeSeconds(line); seconds.has_value()) {
                const double partProgress = secondDurationSeconds > 0.0 ? clamp01(*seconds / secondDurationSeconds) : 1.0;
                const double overall = firstWeight + ((1.0 - firstWeight) * partProgress);
                request.onProgress(overall, formatSplitStatus("Splitting part 2/2", overall));
            }
        },
        [&request]() {
            return request.shouldCancel && request.shouldCancel();
        });
    if (!r2.ok) {
        return { false, r2.stdErr, {} };
    }
    if (request.onProgress) {
        request.onProgress(1.0, formatSplitStatus("Split complete", 1.0));
    }
    std::vector<std::filesystem::path> outputs{ first, second };
    if (request.validateOutputSegments) {
        if (const auto error = validateOutputSegments(info, outputs); error.has_value()) {
            return { false, "Validation failed: " + *error, outputs };
        }
    }

    return { true, request.validateOutputSegments ? "Split completed (validated)" : "Split completed", outputs };
}

SplitResult SplitEngine::splitBySize(const SplitRequest& request, int64_t sizeBytes) const {
    if (!waitIfPaused(request)) {
        return { false, "Canceled", {} };
    }
    if (sizeBytes <= 0) {
        return { false, "Invalid target size", {} };
    }

    std::error_code sizeError;
    const auto inputSize = static_cast<int64_t>(std::filesystem::file_size(request.inputPath, sizeError));
    if (sizeError) {
        return { false, "Unable to determine input file size", {} };
    }

    if (request.minimumFileSizeBytes.has_value() && inputSize < *request.minimumFileSizeBytes) {
        return { true, "Skipped: input size is below minimum file size threshold", {} };
    }

    const bool thresholdSplitMode = request.splitMode == SplitMode::ExactSize || request.splitMode == SplitMode::TorrentPreset;
    if (thresholdSplitMode && !request.unrestrictedMode && inputSize < sizeBytes) {
        return { true, "Skipped: input size is below target split size", {} };
    }

    int64_t targetBytes = request.splitMode == SplitMode::HalfBySize
        ? std::max<int64_t>(1, inputSize / 2)
        : sizeBytes;

    if (targetBytes <= 0) {
        return { false, "Invalid target size", {} };
    }

    int chunks = 1;
    if (request.splitMode == SplitMode::HalfBySize) {
        chunks = 2;
    } else {
        chunks = static_cast<int>((inputSize + targetBytes - 1) / targetBytes);
        chunks = std::max(chunks, 1);
    }

    const auto info = mediaProbe_.probe(request.inputPath);
    if (info.durationSeconds <= 0.0) {
        return { false, "Unable to determine media duration", {} };
    }

    std::filesystem::create_directories(request.outputDirectory);
    const auto stem = request.inputPath.stem().string();
    const auto ext = request.inputPath.extension().string();
    const double safeDuration = std::max(0.001, info.durationSeconds);
    std::vector<std::filesystem::path> outputs;
    outputs.reserve(static_cast<std::size_t>(chunks));

    auto buildSegmentArgs = [&](double startSeconds,
                                double segmentDurationSeconds,
                                const std::filesystem::path& output,
                                bool withProgress) {
        std::vector<std::string> args = {
            "-hide_banner", "-nostats"
        };
        if (withProgress) {
            args.push_back("-progress");
            args.push_back("pipe:2");
        }
        args.push_back("-y");

        if (request.fastSeekEnabled) {
            args.push_back("-ss");
            args.push_back(std::to_string(startSeconds));
            appendHwAccelArgs(args, request);
            args.push_back("-i");
            args.push_back(request.inputPath.string());
        } else {
            appendHwAccelArgs(args, request);
            args.push_back("-i");
            args.push_back(request.inputPath.string());
            args.push_back("-ss");
            args.push_back(std::to_string(startSeconds));
        }

        args.push_back("-t");
        args.push_back(std::to_string(segmentDurationSeconds));
        args.push_back("-map");
        args.push_back("0");
        appendCodecArgs(args, request);

        args.push_back("-avoid_negative_ts");
        args.push_back("make_zero");
        appendThreadArgs(args, request);
        args.push_back(output.string());
        return args;
    };

    double currentStartSeconds = 0.0;
    for (int index = 0; index < chunks; ++index) {
        if (!waitIfPaused(request)) {
            return { false, "Canceled", outputs };
        }

        const int remainingParts = chunks - index;
        const auto output = request.outputDirectory / formatOutputName(request, stem, index + 1, ext);
        const double remainingDuration = std::max(0.0, safeDuration - currentStartSeconds);

        double segmentEndSeconds = safeDuration;
        if (index < chunks - 1) {
            const double minBoundary = currentStartSeconds + 0.001;
            const double maxBoundary = std::max(minBoundary, safeDuration - (static_cast<double>(remainingParts - 1) * 0.001));

            SizePrecisionEngine::SearchConfig config;
            config.targetBytes = targetBytes;
            config.toleranceBytes = 8LL * 1024LL * 1024LL;
            config.minTimeSeconds = minBoundary;
            config.maxTimeSeconds = maxBoundary;
            config.maxIterations = 20;

            const auto tempOutput = request.outputDirectory / (stem + "_estimate_" + std::to_string(index + 1) + ext);
            auto evaluator = [&](double splitTimeSeconds) -> std::optional<int64_t> {
                if (isCanceled(request)) {
                    return std::nullopt;
                }
                const double testDuration = splitTimeSeconds - currentStartSeconds;
                if (testDuration <= 0.0 || !std::isfinite(testDuration)) {
                    return std::nullopt;
                }

                std::error_code cleanupEc;
                std::filesystem::remove(tempOutput, cleanupEc);

                const auto estimateArgs = buildSegmentArgs(currentStartSeconds, testDuration, tempOutput, false);
                const auto estimateRun = process_.runFFmpeg(
                    estimateArgs,
                    nullptr,
                    [&request]() {
                        return request.shouldCancel && request.shouldCancel();
                    });
                if (!estimateRun.ok) {
                    std::filesystem::remove(tempOutput, cleanupEc);
                    return std::nullopt;
                }

                std::error_code sizeEc;
                const auto measuredSize = std::filesystem::file_size(tempOutput, sizeEc);
                std::filesystem::remove(tempOutput, cleanupEc);
                if (sizeEc) {
                    return std::nullopt;
                }
                return static_cast<int64_t>(measuredSize);
            };

            const auto searchResult = precisionEngine_.findSplitTime(config, evaluator);
            if (!searchResult.converged && request.onProgress) {
                const double base = static_cast<double>(index) / static_cast<double>(chunks);
                request.onProgress(base, formatSplitStatus("Size search used best effort boundary", base));
            }

            const double fallbackBoundary = currentStartSeconds + (remainingDuration / static_cast<double>(remainingParts));
            const double measuredBoundary = (searchResult.splitTimeSeconds > minBoundary && std::isfinite(searchResult.splitTimeSeconds))
                ? searchResult.splitTimeSeconds
                : fallbackBoundary;
            double chosenBoundary = std::clamp(measuredBoundary, minBoundary, maxBoundary);

            const auto analysis = gopAnalyzer_.analyze(request.inputPath, chosenBoundary, 45.0);
            const auto nearest = gopAnalyzer_.nearestKeyframe(analysis, chosenBoundary, KeyframeSearchDir::AtOrBefore);
            if (nearest.has_value()) {
                chosenBoundary = std::clamp(nearest->timeSeconds, minBoundary, maxBoundary);
            }
            segmentEndSeconds = chosenBoundary;
        }

        const double segmentDuration = (index == chunks - 1)
            ? std::max(0.001, safeDuration - currentStartSeconds)
            : std::max(0.001, segmentEndSeconds - currentStartSeconds);

        if (request.onProgress) {
            const double base = static_cast<double>(index) / static_cast<double>(chunks);
            request.onProgress(base, formatSplitStatus("Splitting part " + std::to_string(index + 1) + "/" + std::to_string(chunks), base));
        }

        const auto args = buildSegmentArgs(currentStartSeconds, segmentDuration, output, true);
        const auto result = process_.runFFmpeg(
            args,
            [&](const std::string& line) {
                if (!request.onProgress) {
                    return;
                }
                if (const auto seconds = parseProgressTimeSeconds(line); seconds.has_value()) {
                    const double partProgress = segmentDuration > 0.0 ? clamp01(*seconds / segmentDuration) : 1.0;
                    const double overall = (static_cast<double>(index) + partProgress) / static_cast<double>(chunks);
                    request.onProgress(overall, formatSplitStatus("Splitting part " + std::to_string(index + 1) + "/" + std::to_string(chunks), overall));
                }
            },
            [&request]() {
                return request.shouldCancel && request.shouldCancel();
            });

        if (!result.ok) {
            return { false, result.stdErr, outputs };
        }

        outputs.push_back(output);
        if (index < chunks - 1) {
            currentStartSeconds = std::min(safeDuration, currentStartSeconds + segmentDuration);
        }
    }

    if (request.onProgress) {
        request.onProgress(1.0, formatSplitStatus("Split complete", 1.0));
    }
    if (request.validateOutputSegments) {
        if (const auto error = validateOutputSegments(info, outputs); error.has_value()) {
            return { false, "Validation failed: " + *error, outputs };
        }
    }
    return { true, request.validateOutputSegments ? "Split completed (validated)" : "Split completed", outputs };
}

SplitResult SplitEngine::splitByEqualParts(const SplitRequest& request, int partCount) const {
    if (!waitIfPaused(request)) {
        return { false, "Canceled", {} };
    }
    if (partCount < 2) {
        return { false, "Invalid part count", {} };
    }

    const MediaInfo info = mediaProbe_.probe(request.inputPath);
    if (info.durationSeconds <= 0.0) {
        return { false, "Unable to determine media duration", {} };
    }

    std::filesystem::create_directories(request.outputDirectory);
    const auto stem = request.inputPath.stem().string();
    const auto ext = request.inputPath.extension().string();
    const double duration = std::max(0.001, info.durationSeconds);

    std::vector<double> boundaries;
    boundaries.reserve(static_cast<std::size_t>(partCount) + 1);
    boundaries.push_back(0.0);
    for (int i = 1; i < partCount; ++i) {
        const double rawBoundary = duration * (static_cast<double>(i) / static_cast<double>(partCount));
        const double minBoundary = boundaries.back() + 0.001;
        const double maxBoundary = std::max(minBoundary, duration - static_cast<double>(partCount - i) * 0.001);

        double chosenBoundary = std::clamp(rawBoundary, minBoundary, maxBoundary);
        const auto analysis = gopAnalyzer_.analyze(request.inputPath, chosenBoundary, 45.0);
        const auto nearest = gopAnalyzer_.nearestKeyframe(analysis, chosenBoundary, KeyframeSearchDir::AtOrBefore);
        if (nearest.has_value()) {
            chosenBoundary = std::clamp(nearest->timeSeconds, minBoundary, maxBoundary);
        }
        boundaries.push_back(chosenBoundary);
    }
    boundaries.push_back(duration);

    std::vector<std::filesystem::path> outputs;
    outputs.reserve(static_cast<std::size_t>(partCount));

    for (int index = 0; index < partCount; ++index) {
        if (!waitIfPaused(request)) {
            return { false, "Canceled", outputs };
        }

        const double startSeconds = boundaries[static_cast<std::size_t>(index)];
        const double endSeconds = boundaries[static_cast<std::size_t>(index + 1)];
        const double segmentDuration = std::max(0.001, endSeconds - startSeconds);
        const auto output = request.outputDirectory / formatOutputName(request, stem, index + 1, ext);

        if (request.onProgress) {
            const double base = static_cast<double>(index) / static_cast<double>(partCount);
            request.onProgress(base, formatSplitStatus("Splitting part " + std::to_string(index + 1) + "/" + std::to_string(partCount), base));
        }

        std::vector<std::string> args = {
            "-hide_banner", "-nostats", "-progress", "pipe:2", "-y"
        };
        if (request.fastSeekEnabled) {
            args.push_back("-ss");
            args.push_back(std::to_string(startSeconds));
            appendHwAccelArgs(args, request);
            args.push_back("-i");
            args.push_back(request.inputPath.string());
        } else {
            appendHwAccelArgs(args, request);
            args.push_back("-i");
            args.push_back(request.inputPath.string());
            args.push_back("-ss");
            args.push_back(std::to_string(startSeconds));
        }

        args.push_back("-t");
        args.push_back(std::to_string(segmentDuration));
        args.push_back("-map");
        args.push_back("0");
        appendCodecArgs(args, request);
        args.push_back("-avoid_negative_ts");
        args.push_back("make_zero");
        appendThreadArgs(args, request);
        args.push_back(output.string());

        const auto result = process_.runFFmpeg(
            args,
            [&](const std::string& line) {
                if (!request.onProgress) {
                    return;
                }
                if (const auto seconds = parseProgressTimeSeconds(line); seconds.has_value()) {
                    const double partProgress = segmentDuration > 0.0 ? clamp01(*seconds / segmentDuration) : 1.0;
                    const double overall = (static_cast<double>(index) + partProgress) / static_cast<double>(partCount);
                    request.onProgress(overall, formatSplitStatus("Splitting part " + std::to_string(index + 1) + "/" + std::to_string(partCount), overall));
                }
            },
            [&request]() {
                return request.shouldCancel && request.shouldCancel();
            });
        if (!result.ok) {
            return { false, result.stdErr, outputs };
        }

        outputs.push_back(output);
    }

    if (request.onProgress) {
        request.onProgress(1.0, formatSplitStatus("Split complete", 1.0));
    }
    if (request.validateOutputSegments) {
        if (const auto error = validateOutputSegments(info, outputs); error.has_value()) {
            return { false, "Validation failed: " + *error, outputs };
        }
    }
    return { true, request.validateOutputSegments ? "Split completed (validated)" : "Split completed", outputs };
}

std::optional<std::string> SplitEngine::validateOutputSegments(const MediaInfo& sourceInfo,
                                                               const std::vector<std::filesystem::path>& outputs) const {
    if (outputs.empty()) {
        return std::string("no output segments produced");
    }

    double summedDuration = 0.0;
    for (const auto& output : outputs) {
        std::error_code ec;
        if (!std::filesystem::exists(output, ec) || ec) {
            return "missing output file: " + output.string();
        }
        const auto info = mediaProbe_.probe(output);
        if (info.durationSeconds <= 0.0) {
            return "segment unreadable or zero duration: " + output.string();
        }
        if (sourceInfo.width > 0 && sourceInfo.height > 0 && (info.width != sourceInfo.width || info.height != sourceInfo.height)) {
            return "resolution mismatch in segment: " + output.string();
        }
        if (info.audioTracks.size() != sourceInfo.audioTracks.size()) {
            return "audio track count mismatch in segment: " + output.string();
        }
        if (info.subtitleTracks.size() != sourceInfo.subtitleTracks.size()) {
            return "subtitle track count mismatch in segment: " + output.string();
        }
        summedDuration += info.durationSeconds;
    }

    const double tolerance = std::max(2.0, sourceInfo.durationSeconds * 0.02);
    if (std::fabs(summedDuration - sourceInfo.durationSeconds) > tolerance) {
        std::ostringstream message;
        message << "duration mismatch (source=" << sourceInfo.durationSeconds
                << "s, outputs=" << summedDuration << "s)";
        return message.str();
    }
    return std::nullopt;
}

SplitResult SplitEngine::mergeLossless(const std::vector<std::filesystem::path>& segments,
                                       const std::filesystem::path& outputFile) const {
    if (segments.empty()) {
        return { false, "No segments to merge", {} };
    }

    for (std::size_t i = 1; i < segments.size(); ++i) {
        const auto a = mediaProbe_.probe(segments[i - 1]);
        const auto b = mediaProbe_.probe(segments[i]);
        if (a.videoCodec != b.videoCodec || a.width != b.width || a.height != b.height || a.audioTracks.size() != b.audioTracks.size()) {
            return { false, "Segment compatibility validation failed", {} };
        }
    }

    const auto concatList = outputFile.parent_path() / "merge_concat.txt";
    {
        std::ofstream out(concatList, std::ios::binary);
        for (const auto& segment : segments) {
            std::error_code ec;
            auto absolutePath = std::filesystem::absolute(segment, ec);
            if (ec) {
                absolutePath = segment;
            }

            auto normalized = absolutePath.lexically_normal().string();
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            out << "file '" << normalized << "'\n";
        }
    }

    const auto result = process_.runFFmpeg({ "-hide_banner", "-y", "-f", "concat", "-safe", "0", "-i", concatList.string(),
                                             "-c", "copy", outputFile.string() });

    std::error_code error;
    std::filesystem::remove(concatList, error);

    if (!result.ok) {
        return { false, result.stdErr, {} };
    }

    return { true, "Merge completed", { outputFile } };
}

std::string SplitEngine::formatSplitStatus(const std::string& stage, double progress01) {
    const int percentage = static_cast<int>(std::round(clamp01(progress01) * 100.0));
    return stage + " (" + std::to_string(percentage) + "%)";
}

}
