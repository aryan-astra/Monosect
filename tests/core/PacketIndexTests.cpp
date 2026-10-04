// MonoSect — the keyframe and packet index (TODO 051-060).
#include "TestSupport.hpp"
#include "Fixtures.hpp"

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(index_reports_keyframes_and_reordering) {
    const SyntheticSource source = makeSyntheticSource(250, 50, 25, 432);
    const StreamPacketTrack* video = source.index.track(0);
    CHECK(video != nullptr);
    CHECK_EQ(250u, video->size());
    CHECK_EQ(5u, video->keyframeCount());
    CHECK(compareRational(video->keyframeTimes().front(), Rational{0, 1}) == 0);
    CHECK(compareRational(video->keyframeTimes().back(), Rational{8, 1}) == 0);
    CHECK(video->hasReordering());   // B-frames: dts < pts
    CHECK_EQ(0u, video->untimedPacketCount());
}

MONOSECT_TEST(keyframe_lookup_is_inclusive_at_or_before) {
    const SyntheticSource source = makeSyntheticSource(250, 50, 25, 432);
    const StreamPacketTrack* video = source.index.track(0);
    CHECK(compareRational(video->keyframeAtOrBefore((Rational{5, 1})).value(), Rational{4, 1}) == 0);
    CHECK(compareRational(video->keyframeAtOrBefore((Rational{4, 1})).value(), Rational{4, 1}) == 0);
    CHECK(compareRational(video->keyframeAtOrAfter((Rational{5, 1})).value(), Rational{6, 1}) == 0);
    CHECK(video->isKeyframeAt((Rational{4, 1})));
    CHECK(!video->isKeyframeAt((Rational{5, 1})));
}

MONOSECT_TEST(ranges_are_half_open) {
    const SyntheticSource source = makeSyntheticSource(250, 50, 25, 432);
    const StreamPacketTrack* video = source.index.track(0);
    // [2 s, 4 s) contains 50 frames: 2.00 ... 3.96, not 4.00.
    CHECK_EQ(50, video->packetsInRange((Rational{2, 1}), (Rational{4, 1})));
    CHECK_EQ(0, video->packetsInRange((Rational{0, 1}), (Rational{0, 1})));
    CHECK_EQ(250, video->packetsInRange((Rational{0, 1}), (Rational{10, 1})));
    const std::int64_t bytes = video->bytesInRange(Rational{2, 1}, Rational{4, 1});
    CHECK(bytes > 0);
    CHECK(bytes < video->totalPayloadBytes());
}

MONOSECT_TEST(packets_without_timestamps_are_counted_not_guessed) {
    auto root = parseJson(R"({"packets": [
        {"stream_index": 0, "size": "100", "pos": "1000", "flags": "K__"},
        {"stream_index": 0, "size": "100", "pos": "1100", "flags": "__ "}
    ]})");
    CHECK(root.has_value());
    MediaModel model;
    StreamModel video;
    video.index = 0;
    video.kind = StreamKind::Video;
    video.timebase = Rational{1, 1000};
    video.timebaseKnown = true;
    model.streams.push_back(video);

    std::size_t without = 0;
    auto index = parsePacketSection(root.value(), model, &without);
    CHECK(index.has_value());
    CHECK_EQ(2u, without);
    const StreamPacketTrack* track = index.value().track(0);
    CHECK(track != nullptr);
    CHECK_EQ(2u, track->untimedPacketCount());
    CHECK_EQ(0, track->packetsInRange((Rational{0, 1}), (Rational{1, 1})));
}
