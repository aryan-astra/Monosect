#pragma once

#include <string>
#include <vector>

namespace vsm {

class FFmpegProcess final {
public:
    struct Result {
        int exitCode = -1;
        std::string stdOut;
        std::string stdErr;
        bool ok = false;
    };

    explicit FFmpegProcess(std::string ffmpegBinary = "ffmpeg", std::string ffprobeBinary = "ffprobe");

    [[nodiscard]] Result runFFmpeg(const std::vector<std::string>& args) const;
    [[nodiscard]] Result runFFprobe(const std::vector<std::string>& args) const;

private:
    std::string ffmpegBinary_;
    std::string ffprobeBinary_;

    [[nodiscard]] Result runProcess(const std::string& program, const std::vector<std::string>& args) const;
};

}