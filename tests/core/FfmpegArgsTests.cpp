#include "TestSupport.hpp"
#include "engine/FfmpegArgs.hpp"

#include <algorithm>

using namespace monosect::core;

MONOSECT_TEST(encoding_options_follow_output_map_positions_not_input_indexes) {
    SegmentRenderRequest request;
    request.input = "input.mkv";
    request.output = "output.mkv";
    request.end = Rational{2, 1};
    request.copiedStreams = {1};
    request.reencodedStreams = {{0, "h264", StreamKind::Video, 0},
                                {3, "aac", StreamKind::Audio, 96000}};
    const auto args = renderSegmentArgs(request);
    const auto option = [&args](const std::string& key) {
        const auto found = std::find(args.begin(), args.end(), key);
        return found != args.end() && found + 1 != args.end() ? *(found + 1) : std::string{};
    };
    std::vector<std::string> maps;
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (*it == "-map" && it + 1 != args.end()) { maps.push_back(*(it + 1)); }
    }
    CHECK_EQ(3u, maps.size());
    CHECK_EQ(std::string("0:0"), maps[0]);
    CHECK_EQ(std::string("0:1"), maps[1]);
    CHECK_EQ(std::string("0:3"), maps[2]);
    CHECK_EQ(std::string("libx264"), option("-c:0"));
    CHECK_EQ(std::string("aac"), option("-c:2"));
    CHECK_EQ(std::string("96000"), option("-b:2"));
    CHECK(option("-b:a:3").empty());
}
