#include "core/Rational.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <numeric>

namespace monosect::core {
namespace {

constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

std::uint64_t magnitudeOf(std::int64_t value) noexcept {
    return value < 0 ? (~static_cast<std::uint64_t>(value) + 1u) : static_cast<std::uint64_t>(value);
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/// Reads an unsigned decimal integer, returning false on overflow.
bool readUnsigned(std::string_view text, std::size_t& index, std::uint64_t& out) {
    std::uint64_t value = 0;
    const std::size_t start = index;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[index] - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        ++index;
    }
    if (index == start) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

WideProduct wideMultiply(std::int64_t a, std::int64_t b) noexcept {
    WideProduct product;
    product.negative = (a < 0) != (b < 0);
    const std::uint64_t ua = magnitudeOf(a);
    const std::uint64_t ub = magnitudeOf(b);

    const std::uint64_t aLow = ua & 0xFFFFFFFFull;
    const std::uint64_t aHigh = ua >> 32;
    const std::uint64_t bLow = ub & 0xFFFFFFFFull;
    const std::uint64_t bHigh = ub >> 32;

    const std::uint64_t p0 = aLow * bLow;
    const std::uint64_t p1 = aLow * bHigh;
    const std::uint64_t p2 = aHigh * bLow;
    const std::uint64_t p3 = aHigh * bHigh;

    const std::uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFull) + (p2 & 0xFFFFFFFFull);
    product.low = (mid << 32) | (p0 & 0xFFFFFFFFull);
    product.high = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    if (product.low == 0 && product.high == 0) {
        product.negative = false;  // no negative zero
    }
    return product;
}

int compareWide(const WideProduct& a, const WideProduct& b) noexcept {
    const bool aNegative = a.negative && (a.high != 0 || a.low != 0);
    const bool bNegative = b.negative && (b.high != 0 || b.low != 0);
    if (aNegative != bNegative) {
        return aNegative ? -1 : 1;
    }
    if (a.high != b.high) {
        const int comparison = a.high < b.high ? -1 : 1;
        return aNegative ? -comparison : comparison;
    }
    if (a.low != b.low) {
        const int comparison = a.low < b.low ? -1 : 1;
        return aNegative ? -comparison : comparison;
    }
    return 0;
}

namespace {
std::int64_t gcd64(std::uint64_t a, std::uint64_t b) {
    while (b != 0) {
        const std::uint64_t tmp = a % b;
        a = b;
        b = tmp;
    }
    return static_cast<std::int64_t>(std::min<std::uint64_t>(a, static_cast<std::uint64_t>(kInt64Max)));
}
} // namespace

Rational::Rational(std::int64_t num, std::int64_t den) {
    if (den == 0) {
        numerator = 0;
        denominator = 0;  // marks the value invalid; callers must check isValid()
        return;
    }
    if (den < 0) {
        // Negating INT64_MIN is undefined, so fall back to the invalid marker.
        if (num == kInt64Min || den == kInt64Min) {
            numerator = 0;
            denominator = 0;
            return;
        }
        num = -num;
        den = -den;
    }
    const std::uint64_t un = magnitudeOf(num);
    const std::uint64_t ud = magnitudeOf(den);
    const std::int64_t divisor = gcd64(un, ud);
    numerator = num / divisor;
    denominator = den / divisor;
}

double Rational::toDouble() const {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

int compareRational(const Rational& a, const Rational& b) noexcept {
    if (a.denominator == 0 || b.denominator == 0) {
        // Invalid values only compare equal to themselves.
        return (a.denominator == b.denominator && a.numerator == b.numerator) ? 0 : (a.denominator == 0 ? -1 : 1);
    }
    // a.numerator / a.denominator  <=>  b.numerator / b.denominator
    // Both denominators are positive after normalisation.
    return compareWide(wideMultiply(a.numerator, b.denominator), wideMultiply(b.numerator, a.denominator));
}

Result<Rational> addRational(const Rational& a, const Rational& b) {
    const WideProduct left = wideMultiply(a.numerator, b.denominator);
    const WideProduct right = wideMultiply(b.numerator, a.denominator);
    const WideProduct denominator = wideMultiply(a.denominator, b.denominator);
    if (denominator.high != 0) {
        return Error{ErrorCode::Internal, "A time calculation overflowed.",
                     rationalToString(a) + " + " + rationalToString(b), "Use shorter segments."};
    }
    // Turn each 128-bit product into a signed 64-bit value.  The sign flag has to
    // be applied here: `low` alone is a magnitude, so adding two magnitudes would
    // turn "x - x" into "2x".
    const auto toSigned = [](const WideProduct& product) -> std::optional<std::int64_t> {
        if (product.high != 0) {
            return std::nullopt;
        }
        constexpr std::uint64_t kMaxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (product.low > kMaxPositive) {
            return std::nullopt;
        }
        const std::int64_t value = static_cast<std::int64_t>(product.low);
        return product.negative ? -value : value;
    };
    const auto leftValue = toSigned(left);
    const auto rightValue = toSigned(right);
    if (!leftValue.has_value() || !rightValue.has_value()) {
        return Error{ErrorCode::Internal, "A time calculation overflowed.",
                     rationalToString(a) + " + " + rationalToString(b), "Use shorter segments."};
    }
    // The sum itself can still overflow 64 bits; detect it before it happens.
    if (*rightValue > 0 && *leftValue > std::numeric_limits<std::int64_t>::max() - *rightValue) {
        return Error{ErrorCode::Internal, "A time calculation overflowed.",
                     rationalToString(a) + " + " + rationalToString(b), "Use shorter segments."};
    }
    if (*rightValue < 0 && *leftValue < std::numeric_limits<std::int64_t>::min() - *rightValue) {
        return Error{ErrorCode::Internal, "A time calculation overflowed.",
                     rationalToString(a) + " + " + rationalToString(b), "Use shorter segments."};
    }
    return Rational{*leftValue + *rightValue, static_cast<std::int64_t>(denominator.low)};
}

Result<Rational> subtractRational(const Rational& a, const Rational& b) {
    return addRational(a, Rational{-b.numerator, b.denominator});
}

Result<Rational> multiplyRational(const Rational& a, const Rational& b) {
    const WideProduct numerator = wideMultiply(a.numerator, b.numerator);
    const WideProduct denominator = wideMultiply(a.denominator, b.denominator);
    if (numerator.high != 0 || denominator.high != 0) {
        return Error{ErrorCode::Internal, "A time calculation overflowed.",
                     rationalToString(a) + " * " + rationalToString(b), "Use shorter segments."};
    }
    return Rational{numerator.negative ? -static_cast<std::int64_t>(numerator.low)
                                       : static_cast<std::int64_t>(numerator.low),
                    static_cast<std::int64_t>(denominator.low)};
}

Result<Rational> divideRational(const Rational& a, const Rational& b) {
    if (b.numerator == 0) {
        return Error{ErrorCode::Internal, "A time calculation divided by zero.", rationalToString(b)};
    }
    return multiplyRational(a, Rational{b.denominator, b.numerator});
}

std::string rationalToString(const Rational& value) {
    if (value.denominator == 0) {
        return "invalid";
    }
    if (value.denominator == 1) {
        return std::to_string(value.numerator);
    }
    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

Rational TickTime::seconds() const {
    if (!timebase.isValid() || timebase.numerator == 0) {
        return Rational{0, 0};
    }
    // (ticks * numerator) / denominator, reduced first so the product fits.
    const std::uint64_t divisor = static_cast<std::uint64_t>(
        gcd64(magnitudeOf(ticks), magnitudeOf(timebase.denominator)));
    const std::int64_t reducedTicks = ticks / static_cast<std::int64_t>(divisor == 0 ? 1 : divisor);
    const std::int64_t reducedDenominator =
        timebase.denominator / static_cast<std::int64_t>(divisor == 0 ? 1 : divisor);
    const WideProduct product = wideMultiply(reducedTicks, timebase.numerator);
    if (product.high != 0) {
        return Rational{0, 0};  // callers treat this as "unknown", never as zero
    }
    const std::int64_t numerator = product.negative ? -static_cast<std::int64_t>(product.low)
                                                    : static_cast<std::int64_t>(product.low);
    return Rational{numerator, reducedDenominator};
}

TickTime convertTicks(TickTime time, const Rational& targetTimebase, bool roundUp) {
    if (!time.timebase.isValid() || !targetTimebase.isValid() || targetTimebase.numerator == 0) {
        return TickTime{0, targetTimebase};
    }
    // ticks * (source.den / source.num) * (target.num / target.den)
    // Grouped as (ticks * source.den * target.num) / (source.num * target.den) with
    // 64-bit intermediates where safe, falling back to long double only for the
    // sign-safe rounding of values that cannot be represented.
    const WideProduct numeratorA = wideMultiply(time.ticks, time.timebase.denominator);
    const WideProduct denominatorA = wideMultiply(time.timebase.numerator, targetTimebase.denominator);

    if (numeratorA.high == 0 && denominatorA.high == 0 && denominatorA.low != 0) {
        const long double exact =
            (static_cast<long double>(numeratorA.low) * static_cast<long double>(targetTimebase.numerator)) /
            (static_cast<long double>(denominatorA.low));
        long double rounded = roundUp ? std::ceil(exact) : std::floor(exact);
        if (rounded > 9.0e18L) {
            rounded = 9.0e18L;
        }
        return TickTime{static_cast<std::int64_t>(rounded), targetTimebase};
    }

    // Extremely unusual timebases: use the same formula in double precision but
    // report it so the caller can refuse to call the boundary exact.
    const long double fallback = (static_cast<long double>(time.ticks) *
                                  static_cast<long double>(time.timebase.denominator) *
                                  static_cast<long double>(targetTimebase.numerator)) /
                                 (static_cast<long double>(time.timebase.numerator) *
                                  static_cast<long double>(targetTimebase.denominator));
    long double rounded = roundUp ? std::ceil(fallback) : std::floor(fallback);
    if (rounded > 9.0e18L) {
        rounded = 9.0e18L;
    }
    return TickTime{static_cast<std::int64_t>(rounded), targetTimebase};
}

std::string formatDuration(const Rational& seconds) {
    if (!seconds.isValid()) {
        return "unknown";
    }
    // Exact milliseconds, computed from the rational - not from a double.
    // Truncating a double here produced "0:02.057" for 2058/1000 s, and a tool
    // whose whole point is exact durations cannot show a wrong millisecond.
    const bool negative = seconds.numerator < 0;
    const std::uint64_t numerator = magnitudeOf(seconds.numerator);
    const std::uint64_t denominator = magnitudeOf(seconds.denominator);
    std::uint64_t wholeSeconds = denominator == 0 ? 0 : numerator / denominator;
    const std::uint64_t remainder = denominator == 0 ? 0 : numerator % denominator;
    std::uint64_t millis = 0;
    if (remainder != 0 && denominator != 0) {
        // long double holds 64 bits of mantissa: the relative error of this
        // quotient is around 1e-19, so rounding to the nearest millisecond is
        // exact for every value the rational can represent.
        const long double fraction =
            static_cast<long double>(remainder) / static_cast<long double>(denominator);
        millis = static_cast<std::uint64_t>(fraction * 1000.0L + 0.5L);
        if (millis >= 1000) {
            millis -= 1000;
            ++wholeSeconds;
        }
    }

    const std::uint64_t hours = wholeSeconds / 3600;
    const std::uint64_t minutes = (wholeSeconds % 3600) / 60;
    const std::uint64_t secondsPart = wholeSeconds % 60;

    char buffer[64];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%s%llu:%02llu:%02llu.%03llu", negative ? "-" : "",
                      static_cast<unsigned long long>(hours), static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(secondsPart), static_cast<unsigned long long>(millis));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s%llu:%02llu.%03llu", negative ? "-" : "",
                      static_cast<unsigned long long>(minutes), static_cast<unsigned long long>(secondsPart),
                      static_cast<unsigned long long>(millis));
    }
    return buffer;
}

std::string formatSeconds(Rational seconds, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f s", decimals, seconds.toDouble());
    return buffer;
}

Result<Rational> parseDuration(std::string_view text) {
    const std::string_view input = trim(text);
    if (input.empty()) {
        return Error{ErrorCode::ConstraintInvalid, "A duration limit was empty.", "",
                     "Type a duration such as \"90\", \"1:30\" or \"1h30m\"."};
    }

    // Colon form: [hh:]mm:ss[.fff]
    if (input.find(':') != std::string_view::npos) {
        std::int64_t totalSeconds = 0;
        std::int64_t millis = 0;
        std::size_t start = 0;
        int fieldCount = 0;
        while (true) {
            const std::size_t separator = input.find(':', start);
            const std::string_view field =
                input.substr(start, separator == std::string_view::npos ? std::string_view::npos : separator - start);
            if (field.empty()) {
                return Error{ErrorCode::ConstraintInvalid, "That duration has an empty field.",
                             std::string(input), "Use the form hh:mm:ss.fff."};
            }
            std::size_t position = 0;
            std::uint64_t whole = 0;
            if (!readUnsigned(field, position, whole)) {
                return Error{ErrorCode::ConstraintInvalid, "That duration is not a number.",
                             std::string(field), "Use the form hh:mm:ss.fff."};
            }
            totalSeconds = totalSeconds * 60 + static_cast<std::int64_t>(whole);
            if (position < field.size()) {
                if (field[position] != '.') {
                    return Error{ErrorCode::ConstraintInvalid, "That duration has an unexpected character.",
                                 std::string(field), "Use the form hh:mm:ss.fff."};
                }
                ++position;
                std::uint64_t fraction = 0;
                std::size_t digits = 0;
                while (position < field.size() && std::isdigit(static_cast<unsigned char>(field[position]))) {
                    if (digits < 3) {
                        fraction = fraction * 10 + static_cast<std::uint64_t>(field[position] - '0');
                        ++digits;
                    }
                    ++position;
                }
                while (digits < 3) {
                    fraction *= 10;
                    ++digits;
                }
                millis = static_cast<std::int64_t>(fraction);
            }
            ++fieldCount;
            if (separator == std::string_view::npos) {
                break;
            }
            start = separator + 1;
        }
        if (fieldCount > 3) {
            return Error{ErrorCode::ConstraintInvalid, "That duration has too many fields.",
                         std::string(input), "Use the form hh:mm:ss.fff."};
        }
        return Rational{totalSeconds * 1000 + millis, 1000};
    }

    // Unit form: 1h30m, 90s, 500ms, 2.5m ("90.5" alone means seconds).
    long double totalSeconds = 0.0L;
    std::size_t index = 0;
    int fieldCount = 0;
    while (index < input.size()) {
        while (index < input.size() && std::isspace(static_cast<unsigned char>(input[index]))) {
            ++index;
        }
        const std::size_t numberStart = index;
        bool sawDot = false;
        while (index < input.size() &&
               (std::isdigit(static_cast<unsigned char>(input[index])) || input[index] == '.')) {
            sawDot = sawDot || input[index] == '.';
            ++index;
        }
        if (index == numberStart) {
            return Error{ErrorCode::ConstraintInvalid, "That duration could not be read.",
                         std::string(input), "Use \"90\", \"1:30\" or \"1h30m\"."};
        }
        const std::string numberText(input.substr(numberStart, index - numberStart));
        if (sawDot && (numberText.front() == '.' || numberText.back() == '.')) {
            return Error{ErrorCode::ConstraintInvalid, "That duration has a malformed number.",
                         numberText, "Use \"90.5\" or \"1h30m\"."};
        }
        const long double value = std::strtold(numberText.c_str(), nullptr);
        if (!(value >= 0.0L)) {
            return Error{ErrorCode::ConstraintInvalid, "That duration is not a usable number.",
                         numberText, "Use a positive duration."};
        }

        const std::size_t unitStart = index;
        while (index < input.size() && std::isalpha(static_cast<unsigned char>(input[index]))) {
            ++index;
        }
        std::string unit(input.substr(unitStart, index - unitStart));
        std::transform(unit.begin(), unit.end(), unit.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        long double factor = 1.0L;
        if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") {
            factor = 3600.0L;
        } else if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes") {
            factor = 60.0L;
        } else if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" || unit == "seconds" ||
                   unit.empty()) {
            factor = 1.0L;
        } else if (unit == "ms" || unit == "millisecond" || unit == "milliseconds") {
            factor = 0.001L;
        } else {
            return Error{ErrorCode::ConstraintInvalid, "That duration uses an unknown unit.",
                         std::string(input), "Use h, m, s or ms."};
        }
        totalSeconds += value * factor;
        ++fieldCount;
    }
    if (fieldCount == 0) {
        return Error{ErrorCode::ConstraintInvalid, "That duration could not be read.", std::string(input)};
    }
    // Millisecond resolution, rounded down: a limit is a maximum, and container
    // timestamps are never finer than that in practice.  Sub-millisecond input is
    // therefore truncated rather than pretended to be exact.
    const long double millis = std::floor(totalSeconds * 1000.0L);
    if (millis > 9.0e15L) {
        return Error{ErrorCode::ConstraintInvalid, "That duration is too large.", std::string(input)};
    }
    return Rational{static_cast<std::int64_t>(millis), 1000};
}

} // namespace monosect::core
