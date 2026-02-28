#pragma once

#include "engine/FFmpegProcess.hpp"
#include "engine/Types.hpp"

#include <filesystem>

namespace vsm {

class MediaProbe final {
public:
    explicit MediaProbe(FFmpegProcess process = FFmpegProcess());

    [[nodiscard]] MediaInfo probe(const std::filesystem::path& path) const;

private:
    FFmpegProcess process_;
};

}