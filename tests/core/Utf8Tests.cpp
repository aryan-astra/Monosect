// MonoSect — the UTF-8 boundary (core/Utf8Path).
//
// These tests exist because the whole engine stores file names as UTF-8
// std::string, and that is only correct if every conversion at the two edges of
// the program is correct:
//
//   * Windows hands main() an argv already decoded into the ANSI code page, not
//     UTF-8.  On a cp1252 machine a name like "vídeo été 日本.mp4" arrives with
//     the CJK replaced by '?' and the accents as single bytes that are not valid
//     UTF-8.  Handing those bytes to std::filesystem throws
//     filesystem_error("Cannot convert character sequence: Illegal byte
//     sequence"), which is how MonoSect used to greet a perfectly legal file.
//   * std::filesystem::path::u8string() and the narrow constructor are the two
//     links between "narrow means UTF-8" and what the operating system actually
//     does.  They are properties of the standard library, not of this code, so
//     they are pinned here rather than assumed.
//
// The fixture names below are the same ones tests/harness/make_fixtures.py
// creates, so the unit tests and the end-to-end suite test the same strings.
#include "TestSupport.hpp"

#include "core/Utf8Path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace monosect::core;
using namespace monosect::test;

namespace {

/// "vídeo été 日本語.mp4" as UTF-8, written out byte by byte so this test file
/// itself stays pure ASCII and cannot be mangled by an editor, a patch or a
/// code page.  Same characters, same order, same bytes as the fixture.
const std::string kUnicodeName = "v\xC3\xAD""deo \xC3\xA9t\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.mp4";

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("monosect-utf8-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

/// std::u8string -> std::string, so a test can compare against a byte literal
/// without hand-rolling the comparison it is trying to make.
std::string toText(const std::u8string& utf8) {
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

} // namespace

MONOSECT_TEST(well_formed_utf8_is_accepted_and_malformed_sequences_are_not) {
    CHECK(isWellFormedUtf8(""));
    CHECK(isWellFormedUtf8("plain ascii/path.mp4"));
    CHECK(isWellFormedUtf8(kUnicodeName));

    // The valid three-byte encoding of U+FFFD must not be mistaken for the error
    // marker, or every string containing a replacement character would be
    // reported as damaged.
    CHECK(isWellFormedUtf8("\xEF\xBF\xBD"));

    // These are the byte sequences a cp1252 command line actually produces, and
    // they are what made MonoSect throw rather than report a missing file.
    CHECK(!isWellFormedUtf8("vid\xE9""o"));              // one stray cp1252 accent
    CHECK(!isWellFormedUtf8("\xC3"));                     // truncated two-byte lead
    CHECK(!isWellFormedUtf8("\xE6\x97"));                 // truncated three-byte
    CHECK(!isWellFormedUtf8("\xC0\x80"));                 // overlong NUL
    CHECK(!isWellFormedUtf8("\xED\xA0\x80"));             // UTF-16 surrogate D800
    CHECK(!isWellFormedUtf8("\xF5\x80\x80\x80"));         // beyond U+10FFFF
    CHECK(!isWellFormedUtf8("\x80"));                     // stray continuation byte
}

MONOSECT_TEST(utf8_survives_the_round_trip_through_the_filesystem) {
    // This is the link the engine depends on at every call site: a narrow string
    // is UTF-8, std::filesystem::path holds the operating system's own encoding,
    // and narrowing it again yields the original bytes.  If a toolchain ever
    // changes that, this fails here instead of files silently becoming
    // unreachable.
    const std::filesystem::path asPath{kUnicodeName};
    CHECK_EQ(kUnicodeName, utf8FromPath(asPath));
    CHECK_EQ(kUnicodeName, asPath.string());
    CHECK_EQ(kUnicodeName, toText(asPath.u8string()));
    CHECK_EQ(kUnicodeName, utf8FromPath(std::filesystem::path(utf8FromPath(asPath))));
}

MONOSECT_TEST(a_file_whose_name_is_not_ascii_can_be_created_read_and_named) {
    // The defect this fixes was not only a thrown exception: a wrong conversion
    // makes a path that points at nothing, so a file the user can see in
    // Explorer is reported as missing.  The file is therefore created, found,
    // read and removed entirely through narrow UTF-8 strings.
    TempDir directory;
    const std::string relative = kUnicodeName;
    const std::filesystem::path full = directory.path / std::filesystem::path{relative};

    {
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        out << "monosect";
    }
    CHECK(std::filesystem::exists(full));
    CHECK_EQ(std::size_t(8u), static_cast<std::size_t>(std::filesystem::file_size(full)));

    {
        // Scoped so the handle is closed before the removal below: on Windows an
        // open handle makes the file undeletable, which would turn this test's
        // own cleanup into a failure that looks like a MonoSect defect.
        std::ifstream in(full, std::ios::binary);
        std::string contents;
        in >> contents;
        CHECK_EQ(std::string("monosect"), contents);
    }

    // Listing the folder must give the name back in UTF-8, because that is how
    // the watch folder recognises a file it has already seen.
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
        if (entry.path().filename().string() == relative) {
            found = true;
        }
    }
    CHECK(found);

    CHECK(std::filesystem::remove(full));
}

MONOSECT_TEST(widening_and_narrowing_preserve_the_text_and_never_grow_a_path_by_itself) {
    const std::wstring wide = wideFromUtf8(kUnicodeName);
    // 17 characters, all in the basic multilingual plane, so 17 UTF-16 units -
    // against 26 UTF-8 bytes.
    CHECK_EQ(std::size_t(17u), wide.size());
    CHECK_EQ(std::size_t(26u), kUnicodeName.size());
    CHECK_EQ(kUnicodeName, utf8FromWide(wide));

    // The mistake this replaces: std::wstring(s.begin(), s.end()) maps one byte
    // to one wchar_t, so it produces 26 units where the text has 17 - every
    // multi-byte character silently becomes several nonsense characters and the
    // result names a file that cannot exist.
    const std::wstring bytewise(kUnicodeName.begin(), kUnicodeName.end());
    CHECK_EQ(std::size_t(26u), bytewise.size());
    CHECK(bytewise != wide);

    CHECK_EQ(std::wstring(), wideFromUtf8(""));
    CHECK_EQ(std::string(), utf8FromWide(L""));
    CHECK_EQ(std::string("ascii"), utf8FromWide(L"ascii"));
}

MONOSECT_TEST(an_unpaired_surrogate_becomes_a_replacement_rather_than_malformed_utf8) {
    // A lone surrogate cannot be represented in UTF-8.  Emitting malformed UTF-8
    // here would only move the failure to the next conversion; emitting U+FFFD
    // keeps every string in the program well-formed, and the operation that
    // needed the name will simply fail to find the file.
    const std::string converted = utf8FromWide(L"a\xD83D");
    CHECK_EQ(std::size_t(4u), converted.size());   // 'a' plus U+FFFD's three bytes
    CHECK(isWellFormedUtf8(converted));
    CHECK_EQ(converted, utf8FromWide(wideFromUtf8(converted)));
}

MONOSECT_TEST(the_program_arguments_arrive_as_utf8_even_though_argv_does_not) {
#if defined(_WIN32)
    // The wide command line is the authoritative one, so what this returns must
    // match it exactly - and it must be well-formed UTF-8, which main's own argv
    // is not on a machine whose ANSI code page is not UTF-8.
    int wideCount = 0;
    LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount);
    CHECK(wide != nullptr);
    if (wide != nullptr) {
        const std::vector<std::string> arguments = utf8CommandLineArguments(0, nullptr);
        CHECK_EQ(static_cast<std::size_t>(wideCount), arguments.size());
        CHECK(!arguments.empty());
        CHECK_EQ(utf8FromWide(wide[0]), arguments.front());
        for (const std::string& argument : arguments) {
            CHECK(isWellFormedUtf8(argument));
        }
        ::LocalFree(wide);
    }
#else
    // POSIX: argv already is UTF-8, so the function is argv with the elements
    // copied, and an empty argv stays empty.
    char program[] = "monosect";
    char* argv[] = {program};
    const std::vector<std::string> arguments = utf8CommandLineArguments(1, argv);
    CHECK_EQ(std::size_t(1u), arguments.size());
    CHECK_EQ(std::string("monosect"), arguments.front());
    CHECK(utf8CommandLineArguments(0, nullptr).empty());
#endif
}