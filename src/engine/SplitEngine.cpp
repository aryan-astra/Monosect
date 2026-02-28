#include "engine/SplitEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

namespace vsm {

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

    const auto r1 = process_.runFFmpeg({ "-hide_banner", "-y", "-i", request.inputPath.string(),
                                         "-to", splitPoint, "-c", "copy", first.string() });
    if (!r1.ok) {
        return { false, r1.stdErr, {} };
    }

    const auto r2 = process_.runFFmpeg({ "-hide_banner", "-y", "-ss", splitPoint, "-i", request.inputPath.string(),
                                         "-c", "copy", second.string() });
    if (!r2.ok) {
        return { false, r2.stdErr, {} };
    }

    return { true, "Split completed", { first, second } };
}

SplitResult SplitEngine::splitBySize(const SplitRequest& request, int64_t sizeBytes) const {
    if (sizeBytes < 0) {
        return { false, "Invalid target size", {} };
    }

    const auto info = mediaProbe_.probe(request.inputPath);
    if (info.durationSeconds <= 0.0) {
        return { false, "Unable to determine media duration", {} };
    }

    SizePrecisionEngine precision;
    SizePrecisionEngine::SearchConfig config;
    config.targetBytes = request.splitMode == SplitMode::HalfBySize
        ? static_cast<int64_t>(std::filesystem::file_size(request.inputPath) / 2)
        : sizeBytes;
    config.toleranceBytes = 1024 * 1024;
    config.minTimeSeconds = 0.0;
    config.maxTimeSeconds = info.durationSeconds;

    std::filesystem::create_directories(request.outputDirectory);
    const auto stem = request.inputPath.stem().string();
    const auto ext = request.inputPath.extension().string();
    const auto nowTick = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto estimatePath = request.outputDirectory / (stem + "_estimate_" + std::to_string(nowTick) + ext);

    const auto result = precision.findSplitTime(config, [&](double candidate) -> std::optional<int64_t> {
        const auto attempt = process_.runFFmpeg({ "-hide_banner", "-y", "-i", request.inputPath.string(),
                                                  "-to", std::to_string(candidate), "-c", "copy", estimatePath.string() });
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

    return splitByTime(request, result.splitTimeSeconds);
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

}