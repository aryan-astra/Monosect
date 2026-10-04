// MonoSect — exact rational timing (TODO 023-030).
#include "TestSupport.hpp"

#include "core/Rational.hpp"

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(rational_arithmetic_is_exact) {
    const Rational half{1, 2};
    const Rational third{1, 3};
    CHECK_EQ(std::string("5/6"), rationalToString(addRational(half, third).value()));
    CHECK_EQ(std::string("1/6"), rationalToString(subtractRational(half, third).value()));
    CHECK_EQ(std::string("1/6"), rationalToString(multiplyRational(half, third).value()));
    CHECK_EQ(std::string("3/2"), rationalToString(divideRational(half, third).value()));

    // The regression that this test exists for: x - x must be zero, not 2x.
    const Rational big{1920 * 1000, 1001};
    CHECK_EQ(0, addRational(big, (Rational{-1920 * 1000, 1001})).value().numerator);
    CHECK(compareRational(subtractRational(big, big).value(), (Rational{0, 1})) == 0);
}

MONOSECT_TEST(rational_comparison_is_exact) {
    CHECK(compareRational((Rational{30000, 1001}), (Rational{30, 1})) < 0);
    CHECK(compareRational((Rational{1, 3}), (Rational{2, 6})) == 0);
    CHECK((Rational{1, 2}) < (Rational{2, 3}));
    CHECK(!(Rational{0, 0}).isValid());
}

MONOSECT_TEST(duration_parsing_and_formatting) {
    CHECK(compareRational(parseDuration("2:00").value(), (Rational{120, 1})) == 0);
    CHECK(compareRational(parseDuration("1:02:03.5").value(), (Rational{7447, 2})) == 0);
    CHECK(compareRational(parseDuration("90").value(), (Rational{90, 1})) == 0);
    CHECK(!parseDuration("").has_value());
    CHECK(!parseDuration("abc").has_value());

    // Millisecond display is exact, not a truncated double: 2058/1000 s is
    // 2.058 s, and a tool that promises exact durations must not print 2.057.
    CHECK_EQ(std::string("0:02.058"), formatDuration((Rational{2058, 1000})));
    CHECK_EQ(std::string("1:00:00.000"), formatDuration((Rational{3600, 1})));
    CHECK_EQ(std::string("0:00.001"), formatDuration((Rational{1, 1000})));
    // Rounding up must not produce a bogus ".1000" or a wrong second.
    CHECK_EQ(std::string("0:00.001"), formatDuration((Rational{999, 1'000'000})));
    CHECK_EQ(std::string("0:01.000"), formatDuration((Rational{9999, 10'000})));
    CHECK_EQ(std::string("-0:00.001"), formatDuration((Rational{-1, 1000})));
}

MONOSECT_TEST(ticks_convert_without_losing_a_frame) {
    // 1/12800 s per tick: the timebase of an MP4 produced by FFmpeg.
    const TickTime tick{12800, Rational{1, 12800}};
    CHECK(compareRational(tick.seconds(), (Rational{1, 1})) == 0);
    const TickTime frame{19181, Rational{1, 12800}};   // 1.498... s
    CHECK_EQ(std::string("19181/12800"), rationalToString(frame.seconds()));
}
