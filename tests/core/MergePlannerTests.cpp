// MonoSect — merge compatibility and the concat manifest (TODO 155-167).
#include "TestSupport.hpp"
#include "Fixtures.hpp"

#include "core/MergePlanner.hpp"

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(parts_of_one_source_are_compatible) {
    const SyntheticSource source = makeSyntheticSource();
    std::vector<MediaModel> models{source.model, source.model, source.model};
    auto plan = planMerge(models, "merged.mp4");
    CHECK(plan.has_value());
    CHECK(plan.value().planned);
    CHECK(plan.value().compatibility.compatible);
    CHECK_EQ(3u, plan.value().inputs.size());
    CHECK(plan.value().expectedDurationKnown);
    CHECK(compareRational(plan.value().expectedDuration, Rational{30, 1}) == 0);
    CHECK_EQ(std::string("mp4"), plan.value().outputExtension);
}

MONOSECT_TEST(a_different_codec_is_refused_with_the_difference_spelled_out) {
    const SyntheticSource first = makeSyntheticSource();
    const SyntheticSource second = makeSyntheticSource();
    MediaModel other = second.model;
    other.streams[0].codecName = "hevc";
    std::vector<MediaModel> models{first.model, other};
    auto plan = planMerge(models, "merged.mp4");
    CHECK(plan.has_value());
    CHECK(!plan.value().planned);
    CHECK(!plan.value().compatibility.compatible);
    CHECK(!plan.value().compatibility.differences.empty());
    CHECK(plan.value().compatibility.differences.front().find("codec") != std::string::npos ||
          plan.value().compatibility.differences.front().find("h264") != std::string::npos);
    CHECK(plan.value().failureExplanation.find("cannot") != std::string::npos);
}

MONOSECT_TEST(a_different_stream_count_is_refused_and_re_encoding_cannot_help) {
    const SyntheticSource withAudio = makeSyntheticSource();
    const SyntheticSource videoOnly = makeSyntheticSource(250, 50, 25, 0, 44100, false);
    std::vector<MediaModel> models{withAudio.model, videoOnly.model};
    auto plan = planMerge(models, "merged.mp4");
    CHECK(plan.has_value());
    CHECK(!plan.value().planned);
    CHECK(!plan.value().compatibility.reencodeCouldHelp);
}

MONOSECT_TEST(one_file_is_not_a_merge) {
    const SyntheticSource source = makeSyntheticSource();
    auto plan = planMerge({source.model}, "merged.mp4");
    CHECK(!plan.has_value());
}

MONOSECT_TEST(the_manifest_escapes_quotes_the_way_ffmpeg_documents) {
    const std::string manifest = concatManifestContents({"a.mp4", "l'apostrophe & spaces.mp4"});
    CHECK(manifest.rfind("ffconcat version 1.0\n", 0) == 0);
    CHECK(manifest.find("file 'a.mp4'\n") != std::string::npos);
    // The documented escaping for a quote inside a quoted path.
    CHECK(manifest.find("file 'l'\\''apostrophe & spaces.mp4'\n") != std::string::npos);
    CHECK(manifest.find("file 'l'apostrophe") == std::string::npos);
}

MONOSECT_TEST(manifest_names_cannot_collide) {
    const std::string name = uniqueManifestName("18db2a70a70f9e646be1c1c5547a");
    CHECK_EQ(std::string(".monosect-merge-18db2a70a70f9e646be1c1c5547a.ffconcat"), name);
    CHECK_EQ(std::string(".monosect-merge-a_b.ffconcat"), uniqueManifestName("a b"));
    CHECK_EQ(std::string(".monosect-merge-job.ffconcat"), uniqueManifestName(""));
}
