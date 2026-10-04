// MonoSect — the one place a narrow string becomes a wide character string.
//
// The engine speaks UTF-8 for file names.  That is a deliberate choice and not an
// accident: FFmpeg takes its arguments as UTF-8, ImGui draws UTF-8, the job
// database is JSON, and the logs are read by text editors.  On POSIX a file name
// is already a byte string, so UTF-8 is free.  On Windows it is not free, because
// the operating system's own file name type is UTF-16 and the C runtime's `argv`
// is in the ANSI code page.
//
// The two mistakes that follow from mixing those worlds, both of which this
// header exists to prevent:
//
//  * Widening a UTF-8 string one byte at a time (`std::wstring(s.begin(),
//    s.end())`) is not a conversion.  Each byte becomes one wchar_t, so "é"
//    (0xC3 0xA9) turns into two CJK-looking characters and 日本 is destroyed.
//    Windows then looks for a file that never existed.  Use `wideFromUtf8`.
//
//  * Trusting `argv` on Windows.  The C runtime has already converted the
//    command line from UTF-16 to the ANSI code page by the time `main` runs,
//    which on this machine is cp1252: any character cp1252 cannot represent is
//    *irrecoverably replaced with '?'*, and characters it can represent arrive as
//    bytes that are not valid UTF-8.  A UTF-8-decoding consumer then either
//    mis-reads the name or throws.  Use `utf8CommandLineArguments`, which reads
//    the UTF-16 command line directly and converts it.
//
// Everything else in the engine already follows the rule these enforce: a
// `std::string` path is UTF-8, `std::filesystem::path` holds UTF-16, and
// `std::filesystem::path::string()` hands back UTF-8.  UnitTests.cpp pins that
// last link, because it is the one link that is a property of the standard
// library rather than of this code.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <filesystem>
#endif

namespace monosect::core {

/// True when `text` is well-formed UTF-8: no overlong encodings, no surrogate
/// halves, nothing above U+10FFFF, and no truncated multi-byte sequence.
///
/// This is a boundary check, not a general string utility.  It answers one
/// question: "can this byte string be shown to the user and handed to
/// std::filesystem without something downstream throwing?"  Invalid input is
/// replaced at the boundary rather than allowed to become a `std::range_error`
/// from deep inside the standard library.
[[nodiscard]] bool isWellFormedUtf8(std::string_view text);

/// UTF-8 -> UTF-16 (std::wstring) for the wide Win32 entry points:
/// CreateProcessW, CreateFileW, GetOpenFileNameW, ShellExecuteW, the Job Object
/// calls, and everything else in <windows.h> that ends in 'W'.
///
/// Bytes that are not well-formed UTF-8 are passed through as U+FFFD rather
/// than throwing: a mangled character in a path is a failed operation that the
/// caller will report, and an exception thrown here would escape from inside
/// error handling.
[[nodiscard]] std::wstring wideFromUtf8(std::string_view utf8);

/// UTF-16 -> UTF-8 for every wide result the product receives from Windows: the
/// file dialogs, the directory watcher, the watched-file contents and any
/// OS-supplied name.
///
/// Unpaired surrogates become U+FFFD.  A lone surrogate cannot be represented in
/// UTF-8, and producing malformed UTF-8 here would only move the failure to the
/// next conversion.
[[nodiscard]] std::string utf8FromWide(std::wstring_view wide);

/// The program's arguments, decoded as UTF-8, with argv[0] first.
///
/// On Windows this re-reads the command line as UTF-16 with CommandLineToArgvW
/// and converts each argument, because `main`'s own `char** argv` has already
/// lost the characters the ANSI code page cannot hold.  Everywhere else this is
/// ordinary `argv`, which is already UTF-8 there.
[[nodiscard]] std::vector<std::string> utf8CommandLineArguments(int argc, char** argv);

/// UTF-8 form of a path, for messages, the job database and FFmpeg arguments.
///
/// `std::filesystem::path::string()` is used deliberately: on Windows
/// libstdc++ defines it to be `u8string()`, and on POSIX the native narrow form
/// is the bytes MonoSect has been carrying all along.  Writing it through one
/// name makes it visible that "narrow means UTF-8" is a single rule rather than
/// an accident repeated at sixty call sites.
[[nodiscard]] std::string utf8FromPath(const std::filesystem::path& path);

} // namespace monosect::core