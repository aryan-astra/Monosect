#pragma once

#include "engine/FFmpegProcess.hpp"

#include <string>
#include <vector>

namespace vsm {

struct HardwareEncoders {
    bool h264Nvenc = false;
    bool hevcNvenc = false;
    bool h264Qsv = false;
    bool h264Amf = false;
    std::vector<std::string> allEncoders;
};

class HardwareDetector final {
public:
    explicit HardwareDetector(FFmpegProcess process = FFmpegProcess());

    [[nodiscard]] HardwareEncoders detect() const;

private:
    FFmpegProcess process_;
};

}