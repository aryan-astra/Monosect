// MonoSect — exact byte quantities.
//
// Every size limit in MonoSect is an integer number of bytes.  There is no
// floating point anywhere in the path between "the user typed 2 GiB" and "the
// file was 2 147 483 648 bytes or fewer".
#pragma once

#include "core/Result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace monosect::core {

using ByteCount = std::int64_t;

constexpr ByteCount kKiB = 1024;
constexpr ByteCount kMiB = 1024 * kKiB;
constexpr ByteCount kGiB = 1024 * kMiB;
constexpr ByteCount kTiB = 1024 * kGiB;

constexpr ByteCount kKB = 1000;
constexpr ByteCount kMB = 1000 * kKB;
constexpr ByteCount kGB = 1000 * kMB;
constexpr ByteCount kTB = 1000 * kGB;

/// Which convention an ambiguous unit was resolved with.  The UI shows this so
/// that "2 GB" is never silently treated as 2 GiB or the other way round.
enum class UnitSystem {
    Decimal,   // KB, MB, GB, TB  (powers of 1000)
    Binary,    // KiB, MiB, GiB, TiB (powers of 1024)
};

struct ByteSizeParse {
    ByteCount bytes = 0;
    UnitSystem system = UnitSystem::Decimal;
    std::string unitText;      // the suffix exactly as typed
    bool usedAmbiguousUnit = false;  // "2G" - resolved as binary, but the user should know
    bool roundedDown = false;        // the typed value was not a whole number of bytes
    std::string canonical;           // "2147483648 bytes (2 GiB)" for confirmation
};

/// Parses "2 GiB", "2GiB", "2 gb", "2000000000", "1.5 GiB", "750 MiB".
/// Returns an error when the text is not a size at all.
///
/// A limit is a *maximum*, so a fractional value is always rounded DOWN: the
/// effective limit can then never be larger than what the user typed.
[[nodiscard]] Result<ByteSizeParse> parseByteSize(std::string_view text);

/// "2,147,483,648 bytes" - the exact form used whenever a limit or a result is
/// shown.  The separators are accepted back by parseByteSize, so the text can be
/// copied and pasted without changing its meaning.
[[nodiscard]] std::string formatBytesExact(ByteCount bytes);

/// "2.00 GiB" / "2.00 GB" - for compact display only, never for a decision.
[[nodiscard]] std::string formatBytesShort(ByteCount bytes, UnitSystem system = UnitSystem::Binary);

/// "2.00 GiB (2,147,483,648 bytes)" - default UI presentation of a hard limit.
[[nodiscard]] std::string formatBytesWithExact(ByteCount bytes, UnitSystem system = UnitSystem::Binary);

/// Saturating helpers so that a hostile file cannot overflow a size computation.
[[nodiscard]] ByteCount saturatingAdd(ByteCount a, ByteCount b) noexcept;
[[nodiscard]] ByteCount saturatingMul(ByteCount a, ByteCount b) noexcept;

/// Renders a count of bytes the way the log uses it: grouped digits, no unit.
[[nodiscard]] std::string groupDigits(std::int64_t value);

} // namespace monosect::core
