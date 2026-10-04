#include "core/DependencyPin.hpp"

#include "core/Sha256.hpp"

#include <array>
#include <cctype>
#include <fstream>

namespace monosect::core {
namespace {

#ifndef MONOSECT_FFMPEG_URL
#define MONOSECT_FFMPEG_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-03-18-14/ffmpeg-N-127142-g12b7b9891b-win64-gpl.zip"
#endif
#ifndef MONOSECT_FFMPEG_VERSION
#define MONOSECT_FFMPEG_VERSION "N-127142-g12b7b9891b (autobuild-2026-10-03-18-14, win64 gpl)"
#endif
#ifndef MONOSECT_FFMPEG_SHA256
#define MONOSECT_FFMPEG_SHA256 "a885f564dee2b60f69ab866c6c89b96ae531fc2ee1f24ff8b5b1a6d29960a96b"
#endif
#ifndef MONOSECT_FFMPEG_EXE_SHA256
#define MONOSECT_FFMPEG_EXE_SHA256 ""
#endif
#ifndef MONOSECT_FFPROBE_EXE_SHA256
#define MONOSECT_FFPROBE_EXE_SHA256 ""
#endif

std::string lowerCase(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'F') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return text;
}

} // namespace

bool isSha256Hex(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    for (const char character : text) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

std::string sha256HexOfFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    Sha256 hash;
    std::string buffer(64 * 1024, '\0');
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read > 0) {
            hash.update(buffer.data(), static_cast<std::size_t>(read));
        }
    }
    if (input.bad()) {
        return {};
    }
    const std::array<uint8_t, 32> digest = hash.digest();
    return toHex(digest.data(), digest.size());
}

std::filesystem::path DependencyPin::sidecarPath(const std::filesystem::path& toolPath) const {
    std::filesystem::path sidecar = toolPath;
    sidecar += ".sha256";
    return sidecar;
}

std::string DependencyPin::describe() const {
    std::string text = name;
    if (!version.empty()) {
        text += " (" + version + ")";
    }
    text += subject == PinSubject::Archive ? ", archive digest" : ", executable digest";
    text += hasPinnedHash() ? ", pinned" : ", not pinned by this build";
    return text;
}

DependencyPin pinnedArchive() {
    DependencyPin pin;
    pin.name = "the FFmpeg archive";
    pin.version = MONOSECT_FFMPEG_VERSION;
    pin.url = MONOSECT_FFMPEG_URL;
    pin.sha256 = lowerCase(MONOSECT_FFMPEG_SHA256);
    pin.subject = PinSubject::Archive;
    return pin;
}

DependencyPin pinnedFfmpeg() {
    DependencyPin pin;
    pin.name = "ffmpeg.exe";
    pin.version = MONOSECT_FFMPEG_VERSION;
    pin.sha256 = lowerCase(MONOSECT_FFMPEG_EXE_SHA256);
    pin.subject = PinSubject::Executable;
    return pin;
}

DependencyPin pinnedFfprobe() {
    DependencyPin pin;
    pin.name = "ffprobe.exe";
    pin.version = MONOSECT_FFMPEG_VERSION;
    pin.sha256 = lowerCase(MONOSECT_FFPROBE_EXE_SHA256);
    pin.subject = PinSubject::Executable;
    return pin;
}

const char* dependencyCheckOutcomeName(DependencyCheckOutcome outcome) noexcept {
    switch (outcome) {
        case DependencyCheckOutcome::HashMatchesPin: return "matches the pinned release";
        case DependencyCheckOutcome::HashMismatchPin: return "does not match the pinned release";
        case DependencyCheckOutcome::RecordedOnFirstUse: return "hash recorded";
        case DependencyCheckOutcome::HashMatchesRecord: return "matches the recorded download";
        case DependencyCheckOutcome::HashMismatchRecord: return "differs from the recorded download";
        case DependencyCheckOutcome::FileUnreadable: return "file unreadable";
        case DependencyCheckOutcome::InvalidExpectedHash: return "the expected hash is not a SHA-256";
    }
    return "unknown";
}

DependencyCheck verifyToolFile(const std::filesystem::path& toolPath, const DependencyPin& pin,
                              bool recordWhenUnpinned) {
    DependencyCheck check;
    check.actualSha256 = sha256HexOfFile(toolPath);
    if (check.actualSha256.empty()) {
        check.outcome = DependencyCheckOutcome::FileUnreadable;
        check.message = toolPath.empty()
                            ? "No " + pin.name + " was found."
                            : "The file could not be read: " + toolPath.string();
        if (toolPath.empty()) {
            check.message = "MonoSect could not read " + pin.name + ", because no file was given to it.";
        }
        return check;
    }

    if (pin.hasPinnedHash()) {
        if (!isSha256Hex(pin.sha256)) {
            check.outcome = DependencyCheckOutcome::InvalidExpectedHash;
            check.expectedSha256 = pin.sha256;
            check.message = "The pinned SHA-256 for " + pin.name + " is not a valid SHA-256 digest, so " +
                            pin.name + " cannot be identified and will not be used.";
            return check;
        }
        check.expectedSha256 = pin.sha256;
        if (lowerCase(check.actualSha256) == check.expectedSha256) {
            check.outcome = DependencyCheckOutcome::HashMatchesPin;
            check.message = pin.name + " is the pinned release" + (pin.version.empty() ? "" : " (" + pin.version + ")") +
                            ".";
            return check;
        }
        check.outcome = DependencyCheckOutcome::HashMismatchPin;
        check.message = pin.name + " is not the pinned release (expected " + pin.sha256 + ", found " +
                        check.actualSha256 + "). MonoSect will not run a tool it cannot identify.";
        return check;
    }

    // Nothing pinned for this file: trust on first use.  The hash of the file
    // that was just obtained is recorded and enforced from then on, so a binary
    // substituted later is caught.
    const std::filesystem::path sidecar = pin.sidecarPath(toolPath);
    std::ifstream recorded(sidecar, std::ios::binary);
    if (recorded) {
        std::string expected;
        std::getline(recorded, expected);
        while (!expected.empty() &&
               (expected.back() == '\n' || expected.back() == '\r' || expected.back() == ' ')) {
            expected.pop_back();
        }
        check.expectedSha256 = lowerCase(expected);
        if (!isSha256Hex(check.expectedSha256)) {
            check.outcome = DependencyCheckOutcome::InvalidExpectedHash;
            check.message = "The recorded hash for " + pin.name + " is damaged: " + sidecar.string() +
                            ". Delete that file to record a new download.";
            return check;
        }
        if (lowerCase(check.actualSha256) == check.expectedSha256) {
            check.outcome = DependencyCheckOutcome::HashMatchesRecord;
            check.message = pin.name + " matches the recorded download.";
            return check;
        }
        check.outcome = DependencyCheckOutcome::HashMismatchRecord;
        check.message = pin.name + " differs from the one MonoSect obtained earlier (recorded " +
                        check.expectedSha256 + ", found " + check.actualSha256 + ").";
        return check;
    }

    if (!recordWhenUnpinned) {
        check.outcome = DependencyCheckOutcome::HashMatchesRecord;
        check.message = pin.name + " has no pinned digest and nothing recorded yet; treated as new.";
        return check;
    }

    std::ofstream writer(sidecar, std::ios::binary | std::ios::trunc);
    if (!writer) {
        check.outcome = DependencyCheckOutcome::FileUnreadable;
        check.message = pin.name + " could not be fingerprinted: " + sidecar.string() + " is not writable.";
        return check;
    }
    writer << check.actualSha256 << "\n";
    writer.close();
    check.outcome = DependencyCheckOutcome::RecordedOnFirstUse;
    check.message = "No SHA-256 is pinned for " + pin.name + " by this build; its hash was recorded as " +
                    check.actualSha256 + " and will be enforced from now on.";
    return check;
}

bool forgetRecordedToolHash(const std::filesystem::path& toolPath, const DependencyPin& pin) {
    std::error_code error;
    return std::filesystem::remove(pin.sidecarPath(toolPath), error) && !error;
}

} // namespace monosect::core
