#include "core/Bytes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace monosect::core {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

struct UnitFactor {
    const char* suffix;
    double factor;
    UnitSystem system;
    bool ambiguous;
};

// Longest suffix first: the parser walks this table and takes the first match.
constexpr UnitFactor kUnits[] = {
    {"tib", 1024.0 * 1024 * 1024 * 1024, UnitSystem::Binary, false},
    {"gib", 1024.0 * 1024 * 1024, UnitSystem::Binary, false},
    {"mib", 1024.0 * 1024, UnitSystem::Binary, false},
    {"kib", 1024.0, UnitSystem::Binary, false},
    {"tb", 1000.0 * 1000 * 1000 * 1000, UnitSystem::Decimal, false},
    {"gb", 1000.0 * 1000 * 1000, UnitSystem::Decimal, false},
    {"mb", 1000.0 * 1000, UnitSystem::Decimal, false},
    {"kb", 1000.0, UnitSystem::Decimal, false},
    {"t", 1024.0 * 1024 * 1024 * 1024, UnitSystem::Binary, true},
    {"g", 1024.0 * 1024 * 1024, UnitSystem::Binary, true},
    {"m", 1024.0 * 1024, UnitSystem::Binary, true},
    {"k", 1024.0, UnitSystem::Binary, true},
    {"bytes", 1.0, UnitSystem::Binary, false},
    {"byte", 1.0, UnitSystem::Binary, false},
    {"b", 1.0, UnitSystem::Binary, false},
    {"", 1.0, UnitSystem::Binary, false},
};

} // namespace

std::string groupDigits(std::int64_t value) {
    const bool negative = value < 0;
    // Work on the absolute value as an unsigned quantity to avoid UB at INT64_MIN.
    std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(value) + 1u)
                                       : static_cast<std::uint64_t>(value);
    std::string digits = std::to_string(magnitude);
    std::string grouped;
    grouped.reserve(digits.size() + digits.size() / 3 + 1);
    const std::size_t leading = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i == leading || (i > leading && (i - leading) % 3 == 0)) {
            grouped.push_back(',');
        }
        grouped.push_back(digits[i]);
    }
    return negative ? "-" + grouped : grouped;
}

Result<ByteSizeParse> parseByteSize(std::string_view text) {
    const std::string_view input = trim(text);
    if (input.empty()) {
        return Error{ErrorCode::ConstraintInvalid, "A size limit was empty.",
                     "no characters were entered",
                     "Type a size such as \"2 GiB\" or \"2000000000 bytes\"."};
    }

    // Split the numeric prefix from the unit suffix.
    std::size_t index = 0;
    bool seenDigit = false;
    bool seenDot = false;
    while (index < input.size()) {
        const char c = input[index];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            seenDigit = true;
            ++index;
            continue;
        }
        if (c == '.' && !seenDot && seenDigit) {
            seenDot = true;
            ++index;
            continue;
        }
        if (c == ',' && seenDigit) {  // "1,000,000" is accepted
            ++index;
            continue;
        }
        break;
    }
    if (!seenDigit) {
        return Error{ErrorCode::ConstraintInvalid, "That size limit does not start with a number.",
                     std::string("text: \"") + std::string(input) + "\"",
                     "Type a size such as \"2 GiB\" or \"2000000000 bytes\"."};
    }

    std::string numberText;
    numberText.reserve(index);
    for (std::size_t i = 0; i < index; ++i) {
        if (input[i] != ',') {
            numberText.push_back(input[i]);
        }
    }

    const std::string unitText(lower(trim(input.substr(index))));
    const UnitFactor* unit = &kUnits[std::size(kUnits) - 1];
    bool unitFound = false;
    for (const UnitFactor& candidate : kUnits) {
        if (unitText == candidate.suffix) {
            unit = &candidate;
            unitFound = true;
            break;
        }
    }
    if (!unitFound) {
        return Error{ErrorCode::ConstraintInvalid, "That size limit uses an unknown unit.",
                     std::string("unit: \"") + unitText + "\"",
                     "Use B (or \"bytes\"), KiB, MiB, GiB, TiB (binary) or KB, MB, GB, TB (decimal)."};
    }

    double number = 0.0;
    try {
        number = std::stod(numberText);
    } catch (const std::exception&) {
        return Error{ErrorCode::ConstraintInvalid, "That size limit could not be read as a number.",
                     std::string("number: \"") + numberText + "\"",
                     "Type a size such as \"2 GiB\" or \"2000000000 bytes\"."};
    }
    if (!(number >= 0.0) || number > 9.2e18) {
        return Error{ErrorCode::ConstraintInvalid, "That size limit is not a usable number.",
                     std::string("number: \"") + numberText + "\"",
                     "Type a positive size such as \"2 GiB\"."};
    }

    const long double exact = static_cast<long double>(number) * static_cast<long double>(unit->factor);
    if (exact > 9.0e18L) {
        return Error{ErrorCode::ConstraintInvalid, "That size limit is larger than any file can be.",
                     std::string("value: \"") + std::string(input) + "\"",
                     "Type a smaller size."};
    }

    ByteSizeParse out;
    const long double floored = std::floor(exact);
    out.bytes = static_cast<ByteCount>(floored);
    out.roundedDown = (floored != exact);
    out.system = unit->system;
    out.unitText = unitText;
    out.usedAmbiguousUnit = unit->ambiguous;

    if (out.usedAmbiguousUnit) {
        out.canonical = formatBytesWithExact(out.bytes, out.system) + " (the unit \"" + unitText +
                        "\" was read as binary, " + (unit->system == UnitSystem::Binary ? "2^" : "10^") +
                        std::to_string(unit == &kUnits[8] ? 40 : unit == &kUnits[9] ? 30 : unit == &kUnits[10] ? 20 : 10) +
                        ")";
    } else {
        out.canonical = formatBytesWithExact(out.bytes, out.system);
    }
    if (out.bytes == 0) {
        return Error{ErrorCode::SizeLimitTooSmall, "A size limit of zero cannot be used.",
                     "text: \"" + std::string(input) + "\"",
                     "Type a limit of at least 1 MiB."};
    }
    return out;
}

std::string formatBytesExact(ByteCount bytes) {
    return groupDigits(bytes) + " bytes";
}

std::string formatBytesShort(ByteCount bytes, UnitSystem system) {
    const double divisor = system == UnitSystem::Binary ? 1024.0 : 1000.0;
    const char* const* names = nullptr;
    static const char* const binaryNames[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    static const char* const decimalNames[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    names = system == UnitSystem::Binary ? binaryNames : decimalNames;

    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (std::fabs(value) >= divisor && unitIndex < 5) {
        value /= divisor;
        ++unitIndex;
    }
    char buffer[64];
    if (unitIndex == 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld %s", static_cast<long long>(bytes), names[0]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, names[unitIndex]);
    }
    return buffer;
}

std::string formatBytesWithExact(ByteCount bytes, UnitSystem system) {
    return formatBytesShort(bytes, system) + " (" + formatBytesExact(bytes) + ")";
}

ByteCount saturatingAdd(ByteCount a, ByteCount b) noexcept {
    if (b > 0 && a > std::numeric_limits<ByteCount>::max() - b) {
        return std::numeric_limits<ByteCount>::max();
    }
    if (b < 0 && a < std::numeric_limits<ByteCount>::min() - b) {
        return std::numeric_limits<ByteCount>::min();
    }
    return a + b;
}

ByteCount saturatingMul(ByteCount a, ByteCount b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    if (a > 0 && b > 0 && a > std::numeric_limits<ByteCount>::max() / b) {
        return std::numeric_limits<ByteCount>::max();
    }
    if (a > 0 && b < 0 && b < std::numeric_limits<ByteCount>::min() / a) {
        return std::numeric_limits<ByteCount>::min();
    }
    if (a < 0 && b > 0 && a < std::numeric_limits<ByteCount>::min() / b) {
        return std::numeric_limits<ByteCount>::min();
    }
    if (a < 0 && b < 0 && a < std::numeric_limits<ByteCount>::max() / b) {
        return std::numeric_limits<ByteCount>::max();
    }
    return a * b;
}

} // namespace monosect::core
