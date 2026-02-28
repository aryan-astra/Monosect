#pragma once

#include <cstdint>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vsm {

struct GOPEntry {
    int64_t pts = 0;
    int64_t dts = 0;
    double timeSeconds = 0.0;
    int64_t byteOffset = 0;
};

struct GOPAnalysis {
    std::vector<GOPEntry> entries;
    double averageIntervalSeconds = 0.0;
    double densityPerMinute = 0.0;
    bool preciseLosslessPossible = false;
};

struct MediaTrackInfo {
    std::string codec;
    std::string language;
    int index = -1;
};

struct MediaInfo {
    std::filesystem::path path;
    double durationSeconds = 0.0;
    int64_t bitRate = 0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::string videoCodec;
    std::vector<MediaTrackInfo> audioTracks;
    std::vector<MediaTrackInfo> subtitleTracks;
};

enum class SplitMode {
    ExactTime,
    ExactSize,
    HalfByTime,
    HalfBySize,
    TorrentPreset
};

enum class PrecisionMode {
    StrictLossless,
    ExactFrameBoundary,
    Hybrid
};

struct SplitRequest {
    std::filesystem::path inputPath;
    std::filesystem::path outputDirectory;
    SplitMode splitMode = SplitMode::ExactTime;
    PrecisionMode precisionMode = PrecisionMode::StrictLossless;
    std::optional<double> splitTimeSeconds;
    std::optional<int64_t> splitSizeBytes;
    bool unrestrictedMode = false;
    bool useHardwareAcceleration = true;
};

struct SplitResult {
    bool success = false;
    std::string message;
    std::vector<std::filesystem::path> outputSegments;
};

struct SystemMetrics {
    double cpuUsagePercent = 0.0;
    double gpuUsagePercent = 0.0;
    double diskReadMBps = 0.0;
    double diskWriteMBps = 0.0;
    double throughputMBps = 0.0;
    double etaSeconds = 0.0;
};

inline bool isSupportedMediaExtension(const std::filesystem::path& path) {
    static constexpr const char* extensions[] = { ".mp4", ".mkv", ".mov", ".avi", ".flv", ".webm" };
    auto ext = path.extension().string();
    for (auto& ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    for (const char* candidate : extensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
}

inline bool isSafePath(const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    for (const auto& part : normalized) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

}