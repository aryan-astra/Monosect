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

    const auto r1 = process_.runFFmpeg(
        { "-hide_banner", "-nostats", "-progress", "pipe:2", "-y",
          "-i", request.inputPath.string(),
          "-to", splitPoint,
          "-map", "0", "-c", "copy", "-avoid_negative_ts", "make_zero",
          first.string() },
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
    const auto r2 = process_.runFFmpeg(
        { "-hide_banner", "-nostats", "-progress", "pipe:2", "-y",
          "-ss", splitPoint, "-i", request.inputPath.string(),
          "-map", "0", "-c", "copy", "-avoid_negative_ts", "make_zero",
          second.string() },
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

    const auto info = mediaProbe_.probe(request.inputPath);
    if (info.durationSeconds <= 0.0) {
        return { false, "Unable to determine media duration", {} };
    }

    if (request.onProgress) {
        request.onProgress(0.0, "Estimating split position");
    }

    SizePrecisionEngine precision;
    SizePrecisionEngine::SearchConfig config;
    config.targetBytes = request.splitMode == SplitMode::HalfBySize
        ? static_cast<int64_t>(inputSize / 2)
        : sizeBytes;
    config.toleranceBytes = 1024 * 1024;
    config.minTimeSeconds = 0.0;
    config.maxTimeSeconds = info.durationSeconds;

    std::filesystem::create_directories(request.outputDirectory);
    const auto stem = request.inputPath.stem().string();
    const auto ext = request.inputPath.extension().string();
    const auto nowTick = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto estimatePath = request.outputDirectory / (stem + "_estimate_" + std::to_string(nowTick) + ext);
    std::size_t attempts = 0;

    const auto result = precision.findSplitTime(config, [&](double candidate) -> std::optional<int64_t> {
        if (!waitIfPaused(request)) {
            return std::nullopt;
        }
        ++attempts;
        if (request.onProgress) {
            const double phaseProgress = clamp01(static_cast<double>(attempts) / static_cast<double>(config.maxIterations));
            request.onProgress(phaseProgress * 0.45, "Estimating split position (pass " + std::to_string(attempts) + ")");
        }

        const auto attempt = process_.runFFmpeg({ "-hide_banner", "-y", "-i", request.inputPath.string(),
                                                  "-to", std::to_string(candidate), "-map", "0", "-c", "copy",
                                                  "-avoid_negative_ts", "make_zero", estimatePath.string() },
            nullptr,
            [&request]() {
                return request.shouldCancel && request.shouldCancel();
            });
        if (!attempt.ok || !std::filesystem::exists(estimatePath)) {
            return std::nullopt;
        }
        return static_cast<int64_t>(std::filesystem::file_size(estimatePath));
    });

    if (std::filesystem::exists(estimatePath)) {
        std::filesystem::remove(estimatePath);
    }

    if (!result.converged && result.splitTimeSeconds <= 0.0) {
        return { false, "Precision split did not converge", {} };
    }

    SplitRequest finalRequest = request;
    if (request.onProgress) {
        request.onProgress(0.5, "Starting split using estimated position");
        finalRequest.onProgress = [&](double progress01, const std::string& status) {
            const double mapped = 0.5 + (clamp01(progress01) * 0.5);
            request.onProgress(mapped, status);
        };
    }

    return splitByTime(finalRequest, result.splitTimeSeconds);
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
