// MonoSect — JSON parsing (used for every ffprobe result and the job database).
#include "TestSupport.hpp"

#include "core/Json.hpp"

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(json_round_trip) {
    auto parsed = parseJson(R"({"a": 1, "b": "two", "c": [true, null], "d": {"e": 2.5}})");
    CHECK(parsed.has_value());
    const JsonValue& root = parsed.value();
    CHECK(root.isObject());
    CHECK_EQ(std::string("1"), toText(*root.find("a")->asInt64()));
    CHECK_EQ(std::string("two"), root.stringOr("b"));
    CHECK_EQ(2u, root.find("c")->size());
    CHECK(root.find("c")->at(0)->asBool().value());
    CHECK(root.find("c")->at(1)->isNull());
    CHECK_NEAR(2.5, root.find("d")->find("e")->asDouble().value(), 1e-9);
    const std::string dumped = root.dump();
    auto again = parseJson(dumped);
    CHECK(again.has_value());
    CHECK_EQ(std::string("two"), again.value().stringOr("b"));
}

MONOSECT_TEST(json_rejects_malformed_input) {
    CHECK(!parseJson("{").has_value());
    CHECK(!parseJson("{\"a\": }").has_value());
    CHECK(!parseJson("[1,2,]").has_value());
    CHECK(!parseJson("{} trailing").has_value());
}

MONOSECT_TEST(json_lines_skip_blank_lines_and_escape_strings) {
    auto lines = parseJsonLines("{\"n\":1}\n\n{\"n\":2}\n");
    CHECK(lines.has_value());
    CHECK_EQ(2u, lines.value().size());
    CHECK_EQ(std::string("2"), toText(*lines.value()[1].find("n")->asInt64()));

    CHECK_EQ(std::string("\"a\\\"b\\\\c\\nd\""), "\"" + jsonEscape("a\"b\\c\nd") + "\"");
}
