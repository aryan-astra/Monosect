// MonoSect — output names that cannot collide or break Windows (TODO 128-132).
#include "TestSupport.hpp"

#include "core/OutputNaming.hpp"

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(placeholders_are_filled_and_unknown_ones_refused) {
    NamingPolicy policy;
    policy.pattern = "{name} - part {index:03}";
    NamingContext context;
    context.sourceStem = "holiday";
    context.extension = "mp4";
    context.index = 7;
    context.total = 12;
    auto name = renderOutputName(policy, context);
    CHECK(name.has_value());
    CHECK_EQ(std::string("holiday - part 007.mp4"), name.value());

    policy.pattern = "{name} {unknown}";
    CHECK(!renderOutputName(policy, context).has_value());
}

MONOSECT_TEST(windows_reserved_names_and_trailing_characters_are_repaired) {
    CHECK(isReservedDeviceName("CON"));
    CHECK(isReservedDeviceName("com1"));
    CHECK(!isReservedDeviceName("console"));
    CHECK_EQ(std::string("name"), sanitizeFileName("name..."));
    CHECK_EQ(std::string("name"), sanitizeFileName("name   "));
    CHECK_EQ(std::string("a_b_c_d_e_f"), sanitizeFileName("a/b\\c:d*e?f"));
    CHECK_EQ(std::string("part"), sanitizeFileName("   "));
    CHECK(!sanitizeFileName("a\nb").empty());
    CHECK_EQ(std::string("vidéo été"), sanitizeFileName("vidéo été"));
}

MONOSECT_TEST(collisions_resolve_deterministically) {
    // The first free name wins, and the pattern is predictable for the user.
    auto first = resolveNameCollision("part.mp4", false, [](const std::string&) { return false; });
    CHECK(first.acceptable);
    CHECK_EQ(std::string("part.mp4"), first.finalName);

    int calls = 0;
    auto second = resolveNameCollision("part.mp4", false, [&calls](const std::string& name) {
        ++calls;
        return name == "part.mp4" || name == "part (2).mp4";
    });
    CHECK(second.acceptable);
    CHECK_EQ(std::string("part (3).mp4"), second.finalName);

    auto refusing = resolveNameCollision("part.mp4", false, [](const std::string&) { return true; });
    CHECK(!refusing.acceptable);
    CHECK(!refusing.explanation.empty());
}

MONOSECT_TEST(extensions_follow_the_container_family) {
    CHECK_EQ(std::string("mp4"), extensionForFormat("mov,mp4,m4a,3gp,3g2,mj2", "video.mp4"));
    CHECK_EQ(std::string("mov"), extensionForFormat("mov,mp4,m4a,3gp,3g2,mj2", "clip.mov"));
    CHECK_EQ(std::string("mkv"), extensionForFormat("matroska,webm", "movie.mkv"));
    CHECK_EQ(std::string("webm"), extensionForFormat("matroska,webm", "clip.webm"));
    CHECK_EQ(std::string("ts"), extensionForFormat("mpegts", "stream.ts"));
    // An extension the format cannot hold falls back to the family's own.
    CHECK_EQ(std::string("mkv"), extensionForFormat("matroska,webm", "movie.mp4"));
}
