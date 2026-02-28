#pragma once

#include "engine/FFmpegProcess.hpp"
#include "engine/Types.hpp"

#include <filesystem>
#include <optional>

namespace vsm {

class GOPAnalyzer final {
public:
    explicit GOPAnalyzer(FFmpegProcess process = FFmpegProcess());

    [[nodiscard]] GOPAnalysis analyze(const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<GOPEntry> nearestKeyframe(const GOPAnalysis& analysis, double timeSeconds) const;

private:
    FFmpegProcess process_;
};

}