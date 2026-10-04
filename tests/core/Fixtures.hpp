// MonoSect — synthetic media used by the unit tests.
//
// The fixtures are built as the ffprobe JSON that the real parser consumes, so a
// planner test exercises the same code path as a real run: parse the probe
// output, build the packet index, plan against it.  Nothing is stubbed out.
#pragma once

#include "core/Json.hpp"
#include "core/MediaModel.hpp"
#include "core/PacketIndex.hpp"
#include "core/ProbeParsing.hpp"

#include <cmath>
#include <string>

namespace monosect::test {

using namespace monosect::core;

struct SyntheticSource {
    MediaModel model;
    PacketIndex index;
};

/// A CFR H.264 + AAC file: `videoFrames` frames at `fps`, a keyframe every
/// `gopFrames` frames, and `audioPackets` audio packets of 1024 samples.
inline SyntheticSource makeSyntheticSource(int videoFrames = 250, int gopFrames = 50, int fps = 25,
                                           int audioPackets = 432, int audioRate = 44100,
                                           bool withAudio = true) {
    const std::string timebase = "1/12800";
    const std::int64_t videoTicks = 12800 / fps;

    JsonValue streams = JsonValue::makeArray();
    {
        JsonValue video = JsonValue::makeObject();
        video.set("index", JsonValue::makeInt(0));
        video.set("codec_type", JsonValue::makeString("video"));
        video.set("codec_name", JsonValue::makeString("h264"));
        video.set("codec_long_name", JsonValue::makeString("H.264 / AVC / MPEG-4 AVC"));
        video.set("profile", JsonValue::makeString("Main"));
        video.set("level", JsonValue::makeInt(31));
        video.set("width", JsonValue::makeInt(320));
        video.set("height", JsonValue::makeInt(240));
        video.set("pix_fmt", JsonValue::makeString("yuv420p"));
        video.set("time_base", JsonValue::makeString(timebase));
        video.set("r_frame_rate", JsonValue::makeString(std::to_string(fps) + "/1"));
        video.set("avg_frame_rate", JsonValue::makeString(std::to_string(fps) + "/1"));
        video.set("start_pts", JsonValue::makeInt(0));
        video.set("start_time", JsonValue::makeString("0.000000"));
        video.set("duration_ts", JsonValue::makeInt(videoTicks * videoFrames));
        video.set("nb_frames", JsonValue::makeInt(videoFrames));
        video.set("has_b_frames", JsonValue::makeInt(2));
        JsonValue disposition = JsonValue::makeObject();
        disposition.set("default", JsonValue::makeInt(1));
        video.set("disposition", std::move(disposition));
        streams.push(std::move(video));
    }
    if (withAudio) {
        JsonValue audio = JsonValue::makeObject();
        audio.set("index", JsonValue::makeInt(1));
        audio.set("codec_type", JsonValue::makeString("audio"));
        audio.set("codec_name", JsonValue::makeString("aac"));
        audio.set("sample_rate", JsonValue::makeInt(audioRate));
        audio.set("channels", JsonValue::makeInt(1));
        audio.set("channel_layout", JsonValue::makeString("mono"));
        audio.set("time_base", JsonValue::makeString("1/" + std::to_string(audioRate)));
        audio.set("start_pts", JsonValue::makeInt(0));
        audio.set("start_time", JsonValue::makeString("0.000000"));
        streams.push(std::move(audio));
    }

    JsonValue format = JsonValue::makeObject();
    format.set("format_name", JsonValue::makeString("mov,mp4,m4a,3gp,3g2,mj2"));
    format.set("format_long_name", JsonValue::makeString("QuickTime / MOV"));
    const double videoSeconds = static_cast<double>(videoFrames) / static_cast<double>(fps);
    format.set("duration", JsonValue::makeString(std::to_string(videoSeconds).substr(0, 8)));
    format.set("size", JsonValue::makeInt(0));
    format.set("start_time", JsonValue::makeString("0.000000"));

    JsonValue packets = JsonValue::makeArray();
    std::int64_t position = 4096;
    for (int frame = 0; frame < videoFrames; ++frame) {
        JsonValue packet = JsonValue::makeObject();
        packet.set("stream_index", JsonValue::makeInt(0));
        packet.set("pts", JsonValue::makeInt(static_cast<std::int64_t>(frame) * videoTicks));
        packet.set("dts", JsonValue::makeInt(static_cast<std::int64_t>(frame) * videoTicks - 2 * videoTicks));
        packet.set("duration", JsonValue::makeInt(videoTicks));
        packet.set("size", JsonValue::makeInt(2000 + (frame % 7) * 137));
        packet.set("pos", JsonValue::makeInt(position));
        packet.set("flags", JsonValue::makeString(frame % gopFrames == 0 ? "K__" : "___"));
        position += 2000 + (frame % 7) * 137;
        packets.push(std::move(packet));
    }
    if (withAudio) {
        const int samplesPerPacket = 1024;
        for (int packet = 0; packet < audioPackets; ++packet) {
            JsonValue item = JsonValue::makeObject();
            item.set("stream_index", JsonValue::makeInt(1));
            item.set("pts", JsonValue::makeInt(static_cast<std::int64_t>(packet) * samplesPerPacket));
            item.set("dts", JsonValue::makeInt(static_cast<std::int64_t>(packet) * samplesPerPacket));
            item.set("duration", JsonValue::makeInt(samplesPerPacket));
            item.set("size", JsonValue::makeInt(180 + (packet % 5) * 3));
            item.set("pos", JsonValue::makeInt(position));
            item.set("flags", JsonValue::makeString("K__"));
            position += 180 + (packet % 5) * 3;
            packets.push(std::move(item));
        }
    }

    // The file size is the byte position the last packet ended at: the packets
    // start after a 4096-byte container header, so the "container structure" of
    // this fixture is exactly that header - realistic for an MP4, and important,
    // because the size model treats everything that is not packet payload as
    // overhead it must never spend twice.
    const ByteCount fileBytes = position;
    format.set("size", JsonValue::makeInt(fileBytes));

    JsonValue root = JsonValue::makeObject();
    root.set("streams", std::move(streams));
    root.set("format", std::move(format));
    root.set("packets", std::move(packets));

    SyntheticSource source;
    Status status{};
    auto model = parseContainerModel(root, "synthetic.mp4", fileBytes);
    if (model.has_value()) {
        source.model = std::move(model.value());
    }
    auto index = parsePacketSection(root, source.model, nullptr);
    if (index.has_value()) {
        source.index = std::move(index.value());
    }
    (void)status;
    return source;
}

/// A constraint with an exact byte limit, in bytes (no floating point anywhere).
template <typename T>
inline ByteCount bytes(T value) {
    return static_cast<ByteCount>(value);
}

} // namespace monosect::test
