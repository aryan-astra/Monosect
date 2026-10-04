// MonoSect — probing the real ffprobe JSON (TODO 041-050).
#include "TestSupport.hpp"
#include "Fixtures.hpp"

#include "core/ProbeParsing.hpp"

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(container_probe_reads_the_documented_fields) {
    const SyntheticSource source = makeSyntheticSource();
    const MediaModel& model = source.model;
    CHECK_EQ(std::string("synthetic.mp4"), model.path);
    CHECK_EQ(2u, model.streams.size());
    CHECK_EQ(StreamKind::Video, model.streams[0].kind);
    CHECK_EQ(std::string("h264"), model.streams[0].codecName);
    CHECK_EQ(320, model.streams[0].width);
    CHECK_EQ(240, model.streams[0].height);
    CHECK_EQ(std::string("yuv420p"), model.streams[0].pixelFormat);
    CHECK_EQ(1, model.streams[0].isDefault ? 1 : 0);
    CHECK_EQ(StreamKind::Audio, model.streams[1].kind);
    CHECK_EQ(44100, model.streams[1].sampleRate.value());
    CHECK_EQ(1, model.streams[1].channels.value());
    CHECK(model.videoStream() != nullptr);
    CHECK_EQ(0, model.videoStream()->index);
}

MONOSECT_TEST(format_names_do_not_decide_the_family_by_accident) {
    // Every MP4 reports "mov,mp4,m4a,3gp,3g2,mj2"; saying M4A for that would be
    // wrong, and it was the bug this overload exists to prevent.
    CHECK_EQ(std::string("MP4/MOV"), friendlyFormatName("mov,mp4,m4a,3gp,3g2,mj2"));
    // "matroska,webm" is the demuxer name for both formats: without a path the
    // honest name is "Matroska".  Only friendlyFormatName(model), which also sees
    // the file name, says MKV or WebM.
    CHECK_EQ(std::string("Matroska"), friendlyFormatName("matroska,webm"));
    CHECK_EQ(std::string("WebM"), friendlyFormatName("webm"));
    CHECK_EQ(std::string("MPEG-TS"), friendlyFormatName("mpegts"));

    const SyntheticSource video = makeSyntheticSource();
    CHECK_EQ(std::string("MP4/MOV"), friendlyFormatName(video.model));

    SyntheticSource audioOnly = makeSyntheticSource(0, 50, 25, 100, 44100, true);
    audioOnly.model.streams.erase(audioOnly.model.streams.begin());
    audioOnly.model.path = "song.m4a";
    CHECK_EQ(std::string("M4A"), friendlyFormatName(audioOnly.model));
}

MONOSECT_TEST(unknown_values_stay_unknown) {
    auto root = parseJson(R"({
      "streams": [{
        "index": 0, "codec_type": "video", "codec_name": "h264",
        "width": 640, "height": 360, "time_base": "1/90000",
        "avg_frame_rate": "0/0", "r_frame_rate": "30000/1001",
        "duration": "N/A", "bit_rate": "N/A", "nb_frames": "N/A"
      }],
      "format": {"format_name": "mpegts", "duration": "N/A", "size": "1234"}
    })");
    CHECK(root.has_value());
    auto model = parseContainerModel(root.value(), "live.ts", 1234);
    CHECK(model.has_value());
    CHECK(!model.value().streams[0].averageFrameRate.isValid());
    CHECK_EQ(std::string("30000/1001"), rationalToString(model.value().streams[0].baseFrameRate));
    CHECK(!model.value().streams[0].bitRate.has_value());
    CHECK(!model.value().streams[0].frameCount.has_value());
    CHECK(!model.value().duration.has_value());
    CHECK_EQ(std::string("unknown"), std::string(durationSourceName(model.value().durationTruth().source)));
    CHECK(!model.value().durationTruth().reliable);
}

MONOSECT_TEST(argument_lists_are_built_without_a_shell) {
    const auto container = probeArguments("C:\\videos\\a file.mp4", true);
    CHECK(container.size() >= 8);
    CHECK_EQ(std::string("-show_streams"), container[container.size() - 4]);
    CHECK_EQ(std::string("C:\\videos\\a file.mp4"), container.back());

    const auto packets = packetIndexArguments("C:\\videos\\a file.mp4");
    CHECK_EQ(std::string("C:\\videos\\a file.mp4"), packets.back());
    for (const std::string& argument : packets) {
        CHECK(argument.find(';') == std::string::npos);   // no shell metacharacters are needed
    }
}
