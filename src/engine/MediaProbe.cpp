#include "engine/MediaProbe.hpp"
#include "engine/ParseUtils.hpp"

#include <sstream>

namespace vsm {

namespace {

double parseFps(const std::string& value) {
    const auto slash = value.find('/');
    if (slash == std::string::npos) {
        return parseDoubleOr(value, 0.0);
    }

    const double num = parseDoubleOr(value.substr(0, slash), 0.0);
    const double den = parseDoubleOr(value.substr(slash + 1), 0.0);
    return den == 0.0 ? 0.0 : (num / den);
}

} // namespace

MediaProbe::MediaProbe(FFmpegProcess process)
    : process_(std::move(process)) {}

MediaInfo MediaProbe::probe(const std::filesystem::path& path) const {
    MediaInfo info;
    info.path = path;

    const auto formatResult = process_.runFFprobe({
        "-v", "error",
        "-show_entries", "format=duration,bit_rate",
        "-of", "default=nw=1:nk=1",
        path.string()
    });

    if (formatResult.ok) {
        std::istringstream lines(formatResult.stdOut);
        std::string duration;
        std::string bitrate;
        std::getline(lines, duration);
        std::getline(lines, bitrate);
        info.durationSeconds = parseDoubleOr(duration, 0.0);
        info.bitRate = parseInt64Or(bitrate, 0);
    }

    const auto streamResult = process_.runFFprobe({
        "-v", "error",
        "-show_entries", "stream=index,codec_type,codec_name,width,height,avg_frame_rate",
        "-of", "csv=p=0",
        path.string()
    });

    if (!streamResult.ok) {
        return info;
    }

    std::istringstream streamLines(streamResult.stdOut);
    std::string line;
    while (std::getline(streamLines, line)) {
        if (line.empty()) {
            continue;
        }

        const auto cols = splitCsv(line);
        if (cols.size() < 3) {
            continue;
        }

        const int index = static_cast<int>(parseInt64Or(cols[0], -1));
        const std::string& type = cols[1];
        const std::string& codec = cols[2];

        if (type == "video" && info.videoCodec.empty()) {
            info.videoCodec = codec;
            info.width = cols.size() > 3 ? static_cast<int>(parseInt64Or(cols[3], 0)) : 0;
            info.height = cols.size() > 4 ? static_cast<int>(parseInt64Or(cols[4], 0)) : 0;
            info.fps = cols.size() > 5 ? parseFps(cols[5]) : 0.0;
        } else if (type == "audio") {
            info.audioTracks.push_back(MediaTrackInfo{ codec, "", index });
        } else if (type == "subtitle") {
            info.subtitleTracks.push_back(MediaTrackInfo{ codec, "", index });
        }
    }

    return info;
}

}
