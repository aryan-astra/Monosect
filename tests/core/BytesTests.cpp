// MonoSect — byte counts and unit parsing (TODO 016-018).
#include "TestSupport.hpp"

#include "core/Bytes.hpp"

#include <limits>

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(byte_sizes_are_exact_integers) {
    const auto gib = parseByteSize("2 GiB");
    CHECK(gib.has_value());
    CHECK_EQ(std::string("2147483648"), toText(gib.value().bytes));
    CHECK(gib.value().system == UnitSystem::Binary);

    const auto gb = parseByteSize("2 GB");
    CHECK(gb.has_value());
    CHECK_EQ(std::string("2000000000"), toText(gb.value().bytes));
    CHECK(gb.value().system == UnitSystem::Decimal);

    // "2G" is ambiguous.  It resolves to the binary unit *and says so*, because a
    // silent 7.4% difference is exactly the kind of thing that breaks a hard
    // limit.
    const auto ambiguous = parseByteSize("2G");
    CHECK(ambiguous.has_value());
    CHECK_EQ(std::string("2147483648"), toText(ambiguous.value().bytes));
    CHECK(ambiguous.value().usedAmbiguousUnit);

    const auto plain = parseByteSize("2147483648");
    CHECK(plain.has_value());
    CHECK_EQ(std::string("2147483648"), toText(plain.value().bytes));
}

MONOSECT_TEST(byte_size_parsing_rejects_nonsense) {
    CHECK(!parseByteSize("").has_value());
    CHECK(!parseByteSize("two gigabytes").has_value());
    CHECK(!parseByteSize("2 ZiB").has_value());
    CHECK(!parseByteSize("-3 MiB").has_value());

    // A fractional byte count is rounded *down* and reported, never up: rounding
    // a limit up would violate it.
    const auto fractional = parseByteSize("1.5 B");
    CHECK(!fractional.has_value() || fractional.value().bytes == 1);
}

MONOSECT_TEST(byte_formatting_is_readable_and_exact) {
    CHECK_EQ(std::string("2.00 GiB"), formatBytesShort(2ull * 1024 * 1024 * 1024));
    CHECK_EQ(std::string("1.00 KB"), formatBytesShort(1000, UnitSystem::Decimal));
    CHECK_EQ(std::string("1.00 MiB (1,048,576 bytes)"), formatBytesWithExact(1024 * 1024));
    CHECK_EQ(std::string("1,023 bytes"), formatBytesExact(1023));
    // The exact form is what a user can paste back: the parser accepts the
    // grouping separators, so the number shown and the number read are the same.
    const auto roundTrip = parseByteSize(formatBytesExact(1'048'576));
    CHECK(roundTrip.has_value());
    if (roundTrip.has_value()) {
        CHECK_EQ(ByteCount{1'048'576}, roundTrip.value().bytes);
    }
    CHECK_EQ(std::string("1,234,567"), groupDigits(1234567));
}

MONOSECT_TEST(saturating_arithmetic_never_wraps) {
    const ByteCount large = 9'000'000'000'000'000'000ull;
    CHECK_EQ(std::numeric_limits<ByteCount>::max(), saturatingAdd(large, large));
    CHECK_EQ(std::numeric_limits<ByteCount>::max(), saturatingMul(large, 1000));
    CHECK_EQ(ByteCount{0}, saturatingAdd(ByteCount{0}, ByteCount{0}));
}
