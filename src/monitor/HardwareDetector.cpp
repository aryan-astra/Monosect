#include "monitor/HardwareDetector.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace vsm {

HardwareDetector::HardwareDetector(FFmpegProcess process)
    : process_(std::move(process)) {}

HardwareEncoders HardwareDetector::detect() const {
    const auto result = process_.runFFmpeg({ "-hide_banner", "-encoders" });
    HardwareEncoders encoders;
    if (!result.ok) {
        return encoders;
    }

    std::istringstream stream(result.stdOut);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find(" V") == std::string::npos) {
            continue;
        }

        std::string lowered = line;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        encoders.h264Nvenc = encoders.h264Nvenc || lowered.find("h264_nvenc") != std::string::npos;
        encoders.hevcNvenc = encoders.hevcNvenc || lowered.find("hevc_nvenc") != std::string::npos;
        encoders.h264Qsv = encoders.h264Qsv || lowered.find("h264_qsv") != std::string::npos;
        encoders.h264Amf = encoders.h264Amf || lowered.find("h264_amf") != std::string::npos;

        encoders.allEncoders.push_back(line);
    }

    return encoders;
}

}