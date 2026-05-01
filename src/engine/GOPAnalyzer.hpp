#pragma once

#include "engine/FFmpegProcess.hpp"
#include "engine/Types.hpp"

#include <filesystem>
#include <optional>

namespace vsm {

enum class KeyframeSearchDir {
    Nearest,
    AtOrBefore,
    AtOrAfter
};

class GOPAnalyzer final {
public:
    explicit GOPAnalyzer(FFmpegProcess process = FFmpegProcess());

    [[nodiscard]] GOPAnalysis analyze(const std::filesystem::path& path,
                                      double nearSeconds = -1.0,
                                      double windowSeconds = 45.0) const;
    [[nodiscard]] std::optional<GOPEntry> nearestKeyframe(const GOPAnalysis& analysis,
                                                          double timeSeconds,
                                                          KeyframeSearchDir dir = KeyframeSearchDir::AtOrBefore) const;

private:
    FFmpegProcess process_;
};

}
