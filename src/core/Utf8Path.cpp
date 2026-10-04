// MonoSect — UTF-8 <-> UTF-16 boundary conversions.  See Utf8Path.hpp for why
// each of these exists; the reasoning lives with the declarations.
#include "core/Utf8Path.hpp"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#endif

namespace monosect::core {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

/// Decodes one UTF-8 sequence starting at `text[offset]`, appending the code
/// point to `out` and returning the number of bytes consumed.  A malformed
/// sequence yields U+FFFD and consumes exactly one byte, so a bad string is
/// resynchronised rather than swallowed whole.
///
/// The accepted forms are the five in RFC 3629 section 3: 1-4 bytes, the
/// shortest form only (so 0xC0 0x80 is rejected as an overlong encoding of
/// U+0000), and no value above U+10FFFF.  Surrogates D800-DFFF are rejected
/// because UTF-16 cannot carry them and Windows would reject them too.
///
/// `wellFormed` distinguishes "the input was malformed" from "the input was the
/// replacement character", which look identical in the return value but mean
/// opposite things to the caller validating a path.
char32_t decodeOneUtf8(std::string_view text, std::size_t& offset, bool& wellFormed) {
    const auto byteAt = [&text](std::size_t index) { return static_cast<unsigned char>(text[index]); };
    const auto continuation = [&text, &byteAt](std::size_t index) {
        return index < text.size() && (byteAt(index) & 0xC0u) == 0x80u;
    };

    if (byteAt(offset) < 0x80u) {
        ++offset;
        wellFormed = true;
        return byteAt(offset - 1);
    }
    if (byteAt(offset) < 0xC2u) {
        // 0x80-0xBF is a stray continuation byte; 0xC0-0xC1 could only ever
        // encode an overlong form.
        ++offset;
        return kReplacement;
    }
    if (byteAt(offset) < 0xE0u) {
        if (continuation(offset + 1)) {
            const char32_t value = ((byteAt(offset) & 0x1Fu) << 6) | (byteAt(offset + 1) & 0x3Fu);
            offset += 2;
            wellFormed = true;
            return value;
        }
        ++offset;
        return kReplacement;
    }
    if (byteAt(offset) < 0xF0u) {
        if (continuation(offset + 1) && continuation(offset + 2)) {
            const char32_t value = ((byteAt(offset) & 0x0Fu) << 12) | ((byteAt(offset + 1) & 0x3Fu) << 6) |
                                   (byteAt(offset + 2) & 0x3Fu);
            // Overlong if below U+0800; a surrogate if in D800-DFFF.
            if (value >= 0x0800 && (value < 0xD800 || value > 0xDFFF)) {
                offset += 3;
                wellFormed = true;
                return value;
            }
        }
        ++offset;
        return kReplacement;
    }
    if (byteAt(offset) < 0xF5u) {
        if (continuation(offset + 1) && continuation(offset + 2) && continuation(offset + 3)) {
            const char32_t value = ((byteAt(offset) & 0x07u) << 18) | ((byteAt(offset + 1) & 0x3Fu) << 12) |
                                   ((byteAt(offset + 2) & 0x3Fu) << 6) | (byteAt(offset + 3) & 0x3Fu);
            if (value >= 0x10000 && value <= 0x10FFFF) {
                offset += 4;
                wellFormed = true;
                return value;
            }
        }
        ++offset;
        return kReplacement;
    }
    // 0xF5-0xFF cannot start a valid sequence.
    ++offset;
    return kReplacement;
}

/// Appends one code point as UTF-8.  Only the POSIX build needs this: on Windows
/// the operating system's own converter is both authoritative and better tested
/// than a hand-written one, so the same conversion happens in one place there.
#if !defined(_WIN32)
void appendUtf8(std::string& out, char32_t value) {
    if (value < 0x80u) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (value >> 6)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else if (value < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (value >> 12)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (value >> 18)));
        out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    }
}
#endif // !_WIN32

#if defined(_WIN32)

/// UTF-16 -> UTF-8 via the OS, which also has the final say on what a lone
/// surrogate means.
std::string wideToUtf8WithWindows(const wchar_t* text, int length) {
    if (length <= 0) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written =
        ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

#endif // _WIN32

} // namespace

bool isWellFormedUtf8(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t start = offset;
        bool wellFormed = false;
        decodeOneUtf8(text, offset, wellFormed);
        if (!wellFormed) {
            return false;
        }
        if (offset <= start) {
            return false;   // defensive: a zero-length step would loop forever
        }
    }
    return true;
}

std::wstring wideFromUtf8(std::string_view utf8) {
#if defined(_WIN32)
    std::wstring wide;
    wide.reserve(utf8.size());
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        bool wellFormed = false;
        const char32_t value = decodeOneUtf8(utf8, offset, wellFormed);
        if (value < 0x10000u) {
            wide.push_back(static_cast<wchar_t>(value));
        } else {
            const char32_t rest = value - 0x10000u;
            wide.push_back(static_cast<wchar_t>(0xD800u + (rest >> 10)));
            wide.push_back(static_cast<wchar_t>(0xDC00u + (rest & 0x3FFu)));
        }
    }
    return wide;
#else
    // POSIX file names are byte strings and MonoSect's convention is that those
    // bytes are UTF-8, so a wchar_t conversion has no useful meaning here; the
    // only callers are the Windows paths above.
    std::wstring wide;
    wide.reserve(utf8.size());
    for (const char c : utf8) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return wide;
#endif
}

std::string utf8FromWide(std::wstring_view wide) {
#if defined(_WIN32)
    // The size query and the conversion are told the exact length rather than -1,
    // so an embedded NUL (which a file name cannot contain, but a corrupted
    // string might) cannot silently truncate the result.
    return wideToUtf8WithWindows(wide.data(), static_cast<int>(wide.size()));
#else
    std::string utf8;
    utf8.reserve(wide.size());
    for (const wchar_t c : wide) {
        if (c >= 0xD800u && c <= 0xDFFFu) {
            appendUtf8(utf8, kReplacement);
        } else {
            appendUtf8(utf8, static_cast<char32_t>(c));
        }
    }
    return utf8;
#endif
}

std::vector<std::string> utf8CommandLineArguments(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 0u);
    for (int i = 0; i < argc; ++i) {
        arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
#if defined(_WIN32)
    // main's argv is not trustworthy here: the C runtime has already decoded the
    // command line from UTF-16 into the ANSI code page, replacing every character
    // that code page cannot represent with '?'.  Those replacements are
    // unrecoverable, so the command line is read again from the wide form and
    // converted once, correctly, to UTF-8 - which is what the rest of the engine
    // expects every narrow string to be.
    int wideCount = 0;
    LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount);
    if (wide != nullptr) {
        arguments.clear();
        arguments.reserve(static_cast<std::size_t>(wideCount));
        for (int i = 0; i < wideCount; ++i) {
            arguments.push_back(utf8FromWide(wide[i] != nullptr ? wide[i] : L""));
        }
        ::LocalFree(wide);
    }
#endif
    return arguments;
}

std::string utf8FromPath(const std::filesystem::path& path) {
    // u8string() is the explicit spelling of the property this function relies
    // on: on Windows libstdc++ returns UTF-8, and on POSIX it returns the native
    // bytes, which are already UTF-8 by this codebase's convention.  Since C++20
    // it is a std::u8string, so the bytes are copied out one at a time.
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

} // namespace monosect::core