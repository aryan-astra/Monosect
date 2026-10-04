#include "core/OutputNaming.hpp"

#include "core/ProbeParsing.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>

namespace monosect::core {
namespace {

constexpr std::size_t kMaxFileNameLength = 200;      // leave room for the directory
constexpr std::size_t kMaxPathLength = 250;          // conservative on purpose

std::string padIndex(int index, int width) {
    std::string text = std::to_string(index);
    while (static_cast<int>(text.size()) < width) {
        text.insert(text.begin(), '0');
    }
    return text;
}

[[maybe_unused]] std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool isReservedStem(const std::string& stemUpper) {
    static const std::set<std::string> reserved = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                                   "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1",
                                                   "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return reserved.count(stemUpper) != 0;
}

} // namespace

std::string extensionForFormat(std::string_view formatNames, std::string_view sourcePath) {
    // Prefer the source's own extension when the format can be written back with
    // it; otherwise fall back to the container family's default extension.
    std::string sourceExtension;
    if (const std::size_t dot = sourcePath.find_last_of('.'); dot != std::string::npos) {
        sourceExtension = lowercase(sourcePath.substr(dot + 1));
    }
    const auto matches = [&sourceExtension](std::initializer_list<const char*> candidates) {
        for (const char* candidate : candidates) {
            if (sourceExtension == candidate) {
                return true;
            }
        }
        return false;
    };
    if (isMatroskaFormat(formatNames)) {
        // "matroska,webm" is what ffprobe reports for every Matroska file, so the
        // format list cannot be used to prefer .webm: WebM cannot hold H.264 or
        // HEVC, and naming such an output .webm would be a lie.  The source's own
        // extension decides; MKV is the safe default because it holds everything.
        if (matches({"mkv", "mka", "webm"})) {
            return sourceExtension;
        }
        return "mkv";
    }
    if (isIsoBaseMediaFormat(formatNames)) {
        if (matches({"mp4", "mov", "m4a", "m4v"})) {
            return sourceExtension;
        }
        return "mp4";
    }
    if (isTransportStreamFormat(formatNames)) {
        return "ts";
    }
    if (formatNames.find("avi") != std::string_view::npos) {
        return "avi";
    }
    if (formatNames.find("flv") != std::string_view::npos) {
        return "flv";
    }
    if (formatNames.find("ogg") != std::string_view::npos) {
        return "ogg";
    }
    if (formatNames.find("wav") != std::string_view::npos) {
        return "wav";
    }
    if (formatNames.find("mp3") != std::string_view::npos) {
        return "mp3";
    }
    // Unknown container: MKV loses the least when a stream is copied into it.
    return "mkv";
}

std::string sanitizeFileName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const unsigned char byte = static_cast<unsigned char>(c);
        // Control characters, the Windows-reserved set, and the POSIX separator
        // are replaced.  Unicode is kept as it is: Windows and Linux both accept
        // UTF-8 names, and dropping user language characters would be worse.
        if (byte < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
            c == '?' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    // Windows strips trailing dots and spaces; strip them here so that the name
    // MonoSect shows is the name that exists.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    if (out.empty()) {
        out = "part";
    }
    return out;
}

bool isReservedDeviceName(std::string_view name) {
    std::string stem(name);
    if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos) {
        stem = stem.substr(0, dot);
    }
    // Windows resolves "CON.txt" to the device as well, and trailing spaces are
    // ignored, so the comparison is exact but on the trimmed, upper-cased stem.
    while (!stem.empty() && stem.back() == ' ') {
        stem.pop_back();
    }
    std::string upper = stem;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return isReservedStem(upper);
}

Result<std::string> renderOutputName(const NamingPolicy& policy, const NamingContext& context) {
    const std::string pattern = policy.pattern.empty() ? std::string("{name} - part {index:03}") : policy.pattern;

    // A literal extension at the end of the pattern is kept as typed; when the
    // pattern has no extension the container extension is appended.
    std::string stemPattern = pattern;
    std::string explicitExtension;
    const std::size_t lastDot = pattern.find_last_of('.');
    if (lastDot != std::string::npos && pattern.find('{', lastDot) == std::string::npos &&
        pattern.find('}', lastDot) == std::string::npos) {
        explicitExtension = pattern.substr(lastDot);
        stemPattern = pattern.substr(0, lastDot);
    }

    std::string out;
    out.reserve(stemPattern.size() + 32);
    std::size_t position = 0;
    bool sawPlaceholder = false;
    while (position < stemPattern.size()) {
        const std::size_t open = stemPattern.find('{', position);
        if (open == std::string::npos) {
            out.append(stemPattern, position, std::string::npos);
            break;
        }
        out.append(stemPattern, position, open - position);
        const std::size_t close = stemPattern.find('}', open);
        if (close == std::string::npos) {
            return Error{ErrorCode::ConstraintInvalid, "The naming pattern has an unclosed placeholder.",
                         "pattern: " + policy.pattern,
                         "Close the placeholder with '}' or remove it."};
        }
        std::string token = stemPattern.substr(open + 1, close - open - 1);
        std::string padding;
        if (const std::size_t colon = token.find(':'); colon != std::string::npos) {
            padding = token.substr(colon + 1);
            token = token.substr(0, colon);
        }
        int width = 0;
        if (!padding.empty()) {
            if (padding.size() > 1 && padding[0] == '0') {
                width = std::atoi(padding.c_str() + 1);
            } else {
                width = std::atoi(padding.c_str());
            }
        }

        sawPlaceholder = true;
        if (token == "name") {
            out += context.sourceStem;
        } else if (token == "index") {
            out += width > 0 ? padIndex(context.index, width) : std::to_string(context.index);
        } else if (token == "total") {
            out += width > 0 ? padIndex(context.total, width) : std::to_string(context.total);
        } else if (token == "start") {
            out += formatDuration(context.startSeconds);
        } else if (token == "end") {
            out += formatDuration(context.endSeconds);
        } else if (token == "duration") {
            out += formatDuration(context.duration);
        } else if (token == "limit") {
            out += context.limitBytes > 0 ? formatBytesShort(context.limitBytes, UnitSystem::Binary) : "";
        } else if (token == "ext") {
            out += context.extension;
        } else if (token == "method") {
            out += context.method;
        } else if (token == "suffix") {
            out += describeSegmentSuffix(context);
        } else {
            return Error{ErrorCode::ConstraintInvalid, "The naming pattern uses an unknown placeholder.",
                         "placeholder: {" + token + "}, pattern: " + policy.pattern,
                         "Available: {name} {index} {index:03} {total} {start} {end} {duration} {limit} "
                         "{method} {suffix} {ext}."};
        }
        position = close + 1;
    }
    if (!sawPlaceholder) {
        return Error{ErrorCode::ConstraintInvalid, "The naming pattern has no placeholder, so every part would "
                                                   "get the same name.",
                     "pattern: " + policy.pattern,
                     "Add {name}, {index} or another placeholder."};
    }

    std::string name = sanitizeFileName(out);
    const std::string extension = explicitExtension.empty() ? ("." + context.extension) : explicitExtension;
    if (name.size() + extension.size() > kMaxFileNameLength) {
        const std::size_t keep = kMaxFileNameLength > extension.size() ? kMaxFileNameLength - extension.size() : 0;
        name = name.substr(0, keep);
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
            name.pop_back();
        }
    }
    if (name.empty()) {
        return Error{ErrorCode::ConstraintInvalid, "The naming pattern produced an empty file name.",
                     "pattern: " + policy.pattern,
                     "Add at least one placeholder such as {name} or {index}."};
    }
    if (isReservedDeviceName(name)) {
        name += "_file";
    }
    return name + extension;
}

std::string describeSegmentSuffix(const NamingContext& context) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s-%s", formatDuration(context.startSeconds).c_str(),
                  formatDuration(context.endSeconds).c_str());
    return buffer;
}

NameCollision resolveNameCollision(std::string_view desiredName, bool overwriteExisting,
                                   const std::function<bool(const std::string&)>& exists) {
    NameCollision collision;
    if (overwriteExisting || !exists(std::string(desiredName))) {
        collision.finalName = std::string(desiredName);
        return collision;
    }
    std::string stem(desiredName);
    std::string extension;
    if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos) {
        extension = stem.substr(dot);
        stem = stem.substr(0, dot);
    }
    for (int attempt = 2; attempt <= 999; ++attempt) {
        const std::string candidate = stem + " (" + std::to_string(attempt) + ")" + extension;
        if (!exists(candidate)) {
            collision.finalName = candidate;
            return collision;
        }
    }
    collision.acceptable = false;
    collision.explanation = "A file with this name exists and 999 alternatives were also taken: " +
                            std::string(desiredName);
    return collision;
}

Result<std::string> validateOutputPath(const std::string& directory, const std::string& fileName) {
    if (fileName.empty()) {
        return Error{ErrorCode::OutputWriteFailed, "The output file name is empty.", "",
                     "Check the naming pattern."};
    }
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += fileName;
    if (path.size() > kMaxPathLength) {
        return Error{ErrorCode::PathTooLong,
                     "The output path is longer than Windows can reliably handle.",
                     "length: " + std::to_string(path.size()) + " characters, limit: " +
                         std::to_string(kMaxPathLength) + ", path: " + path,
                     "Choose a shorter output folder or a shorter naming pattern."};
    }
    return path;
}

} // namespace monosect::core
