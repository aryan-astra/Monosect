#pragma once

#include "engine/FFmpegProcess.hpp"
#include "engine/GOPAnalyzer.hpp"
#include "engine/MediaProbe.hpp"
#include "engine/Types.hpp"
#include "precision/SizePrecisionEngine.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace vsm {

class SplitEngine final {
public:
    SplitEngine();
    explicit SplitEngine(FFmpegProcess process);

    [[nodiscard]] SplitResult split(const SplitRequest& request) const;
    [[nodiscard]] SplitResult mergeLossless(const std::vector<std::filesystem::path>& segments,
                                            const std::filesystem::path& outputFile) const;

private:
    FFmpegProcess process_;
    GOPAnalyzer gopAnalyzer_;
    MediaProbe mediaProbe_;

    static std::string formatSplitStatus(const std::string& stage, double progress01);

    [[nodiscard]] SplitResult splitByTime(const SplitRequest& request, double splitTime) const;
    [[nodiscard]] SplitResult splitBySize(const SplitRequest& request, int64_t sizeBytes) const;
};

}
