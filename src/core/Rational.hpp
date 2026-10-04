// MonoSect — exact rational time arithmetic.
//
// Media timestamps are rational numbers (PTS / timebase).  All duration limits
// and all boundary decisions are compared as exact rationals - never as doubles.
// The only place a double is allowed is at the very end, when a number is turned
// into text for the user.
//
// 64 x 64 -> 128 bit multiplication is implemented by hand so that the code
// behaves identically under MSVC (which has no __int128) and under GCC/Clang.
#pragma once

#include "core/Result.hpp"

#include <compare>
#include <cstdint>
#include <string>

namespace monosect::core {

struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    Rational() = default;
    Rational(std::int64_t value) : numerator(value), denominator(1) {}  // NOLINT(google-explicit-constructor)
    Rational(std::int64_t num, std::int64_t den);

    [[nodiscard]] bool isZero() const noexcept { return numerator == 0; }
    [[nodiscard]] bool isPositive() const noexcept { return numerator > 0; }
    [[nodiscard]] bool isValid() const noexcept { return denominator != 0; }

    /// Display only.  Never feed the result back into a decision.
    [[nodiscard]] double toDouble() const;
};

/// Exact value of `a - b`'s sign, computed with 128-bit intermediates.
[[nodiscard]] int compareRational(const Rational& a, const Rational& b) noexcept;
[[nodiscard]] inline bool operator==(const Rational& a, const Rational& b) noexcept {
    return compareRational(a, b) == 0;
}
[[nodiscard]] inline bool operator!=(const Rational& a, const Rational& b) noexcept {
    return compareRational(a, b) != 0;
}
[[nodiscard]] inline bool operator<(const Rational& a, const Rational& b) noexcept {
    return compareRational(a, b) < 0;
}
[[nodiscard]] inline bool operator>(const Rational& a, const Rational& b) noexcept {
    return compareRational(a, b) > 0;
}
[[nodiscard]] inline bool operator<=(const Rational& a, const Rational& b) noexcept {
    return compareRational(a, b) <= 0;
}
[[nodiscard]] inline bool operator>=(const Rational& a, const Rational& b) noexcept {
    return compareRational(a, b) >= 0;
}

/// Exact arithmetic.  Returns an error instead of overflowing silently.
[[nodiscard]] Result<Rational> addRational(const Rational& a, const Rational& b);
[[nodiscard]] Result<Rational> subtractRational(const Rational& a, const Rational& b);
[[nodiscard]] Result<Rational> multiplyRational(const Rational& a, const Rational& b);
[[nodiscard]] Result<Rational> divideRational(const Rational& a, const Rational& b);

/// "1/1000" or "30000/1001" - the canonical form for logs.
[[nodiscard]] std::string rationalToString(const Rational& value);

/// A timestamp in a specific stream timebase: `ticks / timebase`.
struct TickTime {
    std::int64_t ticks = 0;
    Rational timebase{1, 1};

    [[nodiscard]] Rational seconds() const;
    [[nodiscard]] double secondsAsDouble() const { return seconds().toDouble(); }
    [[nodiscard]] bool valid() const noexcept { return timebase.isValid(); }

    friend bool operator==(const TickTime& a, const TickTime& b) noexcept {
        return a.ticks == b.ticks && compareRational(a.timebase, b.timebase) == 0;
    }
    friend bool operator<(const TickTime& a, const TickTime& b) noexcept {
        return compareRational(a.seconds(), b.seconds()) < 0;
    }
};

/// Converts a timestamp to another timebase, rounding down.  Rounding is explicit
/// because it is exactly what makes a boundary "not exact"; the caller reports it.
[[nodiscard]] TickTime convertTicks(TickTime time, const Rational& targetTimebase, bool roundUp = false);

/// Formats a duration as "1:23:45.678" when it is at least an hour, "3:04.500"
/// otherwise.  Millisecond precision, rounded down.
[[nodiscard]] std::string formatDuration(const Rational& seconds);

/// Formats a duration with second+millisecond precision and the unit, e.g.
/// "3.044 s" - used where a compact form reads better inside a sentence.
[[nodiscard]] std::string formatSeconds(Rational seconds, int decimals = 3);

/// Parses "1500", "90.5", "2:03", "1:02:03.250", "2h30m", "500ms", "1h2m3.5s".
/// Returns an exact Rational in seconds.
[[nodiscard]] Result<Rational> parseDuration(std::string_view text);

// --- portable 128-bit helpers (exposed for tests) -------------------------

struct WideProduct {
    bool negative = false;
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

[[nodiscard]] WideProduct wideMultiply(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] int compareWide(const WideProduct& a, const WideProduct& b) noexcept;

} // namespace monosect::core
