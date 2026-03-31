#include "engine/SplitEngine.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
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

    return { false, "Invalid split request", {} };
}

SplitResult SplitEngine::splitByTime(const SplitRequest& request, double splitTime) const {
    if (!waitIfPaused(request)) {
        return { false, "Canceled", {} };
    }
    if (splitTime < 0.0 || !std::isfinite(splitTime)) {
        return { false, "Invalid split time", {} };
    }

    const auto info = mediaProbe_.probe(request.inputPath);
    if (info.durationSeconds <= 0.0) {
        return { false, "Unable to determine media duration", {} };
    }

    if (splitTime >= info.durationSeconds) {
        splitTime = std::max(0.0, info.durationSeconds - 0.001);
    }

    const auto analysis = gopAnalyzer_.analyze(request.inputPath);
    const auto nearest = gopAnalyzer_.nearestKeyframe(analysis, splitTime);
    if (!nearest.has_value()) {
        return { false, "No keyframe found", {} };
    }

    std::filesystem::create_directories(request.outputDirectory);

    const auto stem = request.inputPath.stem().string();
    const auto ext = request.inputPath.extension().string();
    const auto first = request.outputDirectory / (stem + "_part1" + ext);
    const auto second = request.outputDirectory / (stem + "_part2" + ext);

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
        "-i", request.inputPath.string(),
        "-to", splitPoint,
        "-map", "0"
    };
    if (request.codecOverride.has_value() && *request.codecOverride != "copy") {
        firstArgs.push_back("-c:v");
        firstArgs.push_back(*request.codecOverride);
        firstArgs.push_back("-c:a");
        firstArgs.push_back("copy");
        firstArgs.push_back("-c:s");
        firstArgs.push_back("copy");
    } else {
        firstArgs.push_back("-c");
        firstArgs.push_back("copy");
    }
    firstArgs.push_back("-avoid_negative_ts");
    firstArgs.push_back("make_zero");
    if (request.desiredThreads.has_value() && *request.desiredThreads > 0) {
        firstArgs.push_back("-threads");
        firstArgs.push_back(std::to_string(*request.desiredThreads));
    } else {
        firstArgs.push_back("-threads");
        firstArgs.push_back("0");
    }
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
        secondArgs.push_back("-i");
        secondArgs.push_back(request.inputPath.string());
    } else {
        secondArgs.push_back("-i");
        secondArgs.push_back(request.inputPath.string());
        secondArgs.push_back("-ss");
        secondArgs.push_back(splitPoint);
    }
    secondArgs.push_back("-map");
    secondArgs.push_back("0");
    if (request.codecOverride.has_value() && *request.codecOverride != "copy") {
        secondArgs.push_back("-c:v");
        secondArgs.push_back(*request.codecOverride);
        secondArgs.push_back("-c:a");
        secondArgs.push_back("copy");
        secondArgs.push_back("-c:s");
        secondArgs.push_back("copy");
    } else {
        secondArgs.push_back("-c");
        secondArgs.push_back("copy");
    }
    secondArgs.push_back("-avoid_negative_ts");
    secondArgs.push_back("make_zero");
    if (request.desiredThreads.has_value() && *request.desiredThreads > 0) {
        secondArgs.push_back("-threads");
        secondArgs.push_back(std::to_string(*request.desiredThreads));
    } else {
        secondArgs.push_back("-threads");
        secondArgs.push_back("0");
    }
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

    return { true, "Split completed", { first, second } };
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
    const double baseChunkDuration = safeDuration / static_cast<double>(chunks);
    std::vector<std::filesystem::path> outputs;
    outputs.reserve(static_cast<std::size_t>(chunks));

    for (int index = 0; index < chunks; ++index) {
        if (!waitIfPaused(request)) {
            return { false, "Canceled", outputs };
        }

        const double startSeconds = baseChunkDuration * static_cast<double>(index);
        const double remaining = std::max(0.0, safeDuration - startSeconds);
        const double chunkDuration = (index == chunks - 1)
            ? std::max(0.001, remaining)
            : std::max(0.001, baseChunkDuration);

        const auto output = request.outputDirectory / (stem + "_part" + std::to_string(index + 1) + ext);
        std::vector<std::string> args = {
            "-hide_banner", "-nostats", "-progress", "pipe:2", "-y"
        };
        if (request.fastSeekEnabled) {
            args.push_back("-ss");
            args.push_back(std::to_string(startSeconds));
            args.push_back("-i");
            args.push_back(request.inputPath.string());
        } else {
            args.push_back("-i");
            args.push_back(request.inputPath.string());
            args.push_back("-ss");
            args.push_back(std::to_string(startSeconds));
        }
        args.push_back("-t");
        args.push_back(std::to_string(chunkDuration));
        args.push_back("-map");
        args.push_back("0");
        if (request.codecOverride.has_value() && *request.codecOverride != "copy") {
            args.push_back("-c:v");
            args.push_back(*request.codecOverride);
            args.push_back("-c:a");
            args.push_back("copy");
            args.push_back("-c:s");
            args.push_back("copy");
        } else {
            args.push_back("-c");
            args.push_back("copy");
        }
        args.push_back("-avoid_negative_ts");
        args.push_back("make_zero");
        if (request.desiredThreads.has_value() && *request.desiredThreads > 0) {
            args.push_back("-threads");
            args.push_back(std::to_string(*request.desiredThreads));
        } else {
            args.push_back("-threads");
            args.push_back("0");
        }
        args.push_back(output.string());

        if (request.onProgress) {
            const double base = static_cast<double>(index) / static_cast<double>(chunks);
            request.onProgress(base, formatSplitStatus("Splitting part " + std::to_string(index + 1) + "/" + std::to_string(chunks), base));
        }

        const auto result = process_.runFFmpeg(
            args,
            [&](const std::string& line) {
                if (!request.onProgress) {
                    return;
                }
                if (const auto seconds = parseProgressTimeSeconds(line); seconds.has_value()) {
                    const double partProgress = chunkDuration > 0.0 ? clamp01(*seconds / chunkDuration) : 1.0;
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
    }

    if (request.onProgress) {
        request.onProgress(1.0, formatSplitStatus("Split complete", 1.0));
    }
    return { true, "Split completed", outputs };
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
