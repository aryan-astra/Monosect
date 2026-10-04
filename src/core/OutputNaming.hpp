// MonoSect — output file naming.
//
// Names are produced by a pattern, sanitised for the file system, checked against
// Windows' reserved device names, checked against the maximum path length, and
// never allowed to overwrite an existing file unless the user asked for that.
#pragma once

#include "core/Bytes.hpp"
#include "core/Rational.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace monosect::core {

struct NamingPolicy {
    /// Placeholders: {name} {index} {index:03} {total} {start} {end} {duration}
    /// {limit} {ext} {method}
    std::string pattern = "{name} - part {index:03}";
    bool overwriteExisting = false;
};

struct NamingContext {
    std::string sourceStem;
    std::string extension;          // without the dot
    int index = 1;
    int total = 0;
    Rational startSeconds;
    Rational endSeconds;
    Rational duration;
    ByteCount limitBytes = 0;
    std::string method;
};

/// Renders the file name (no directory).  Returns an error when the pattern
/// contains an unknown placeholder or produces an empty name.
[[nodiscard]] Result<std::string> renderOutputName(const NamingPolicy& policy, const NamingContext& context);

/// Replaces characters that Windows and POSIX file systems reject, and trims
/// trailing dots and spaces (Windows silently strips them, which would make two
/// different names collide).
[[nodiscard]] std::string sanitizeFileName(std::string_view name);

/// True for CON, PRN, AUX, NUL, COM1-9, LPT1-9 (case-insensitive, with or
/// without an extension).
[[nodiscard]] bool isReservedDeviceName(std::string_view name);

/// Extension for the target container, based on the source format family.
[[nodiscard]] std::string extensionForFormat(std::string_view formatNames, std::string_view sourcePath);

struct NameCollision {
    bool acceptable = true;
    std::string finalName;
    std::string explanation;   // filled in when acceptable == false
};

/// Appends " (2)", " (3)", ... until the name is free, or reports that no free
/// name was found within 999 attempts.
[[nodiscard]] NameCollision resolveNameCollision(std::string_view desiredName, bool overwriteExisting,
                                                 const std::function<bool(const std::string&)>& exists);

/// Full path check: length and separator sanity.
[[nodiscard]] Result<std::string> validateOutputPath(const std::string& directory, const std::string& fileName);

/// "1920x1080 25 fps" style suffix used by the default pattern for uniqueness.
[[nodiscard]] std::string describeSegmentSuffix(const NamingContext& context);

} // namespace monosect::core
