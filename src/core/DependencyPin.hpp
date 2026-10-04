// MonoSect — the pinned tools, and proof that the files on disk are the tools
// that were downloaded (TODO 221, TODO 315).
//
// MonoSect runs ffmpeg.exe and ffprobe.exe from its own folder.  That makes those
// two files part of the product's trusted base, so two questions have to have
// answers that do not depend on trust:
//
//   1. **Which build is it?**  A pin records the release the product ships: the
//      URL it came from and the version it claims to be.  The version is *also*
//      read back from the tool itself (`ffmpeg -version`) and stored in every job
//      record, so the recorded version is never just a constant someone edited.
//
//   2. **Is the file still the file we downloaded?**  There are two files to
//      check and they are *different files*, so they need two hashes:
//
//        * the **archive** MonoSect downloads, verified *before* it is opened;
//        * each **executable** after extraction.
//
//      One hash cannot cover both - a 200 MB zip and the 90 MB exe inside it
//      obviously have different digests - and treating them as one thing is how a
//      "verified" toolchain silently deletes the tools it just verified.
//      Verifying the archive first is the primary anchor: everything extracted
//      from a verified archive is determined by that archive.  Verifying the
//      executables as well is defence in depth, and it matters here because
//      extraction on Windows shells out to PowerShell.
//
// When a build does not pin a hash, trust-on-first-use applies *per file*: the
// hash of what was downloaded/recorded is written next to it and enforced from
// then on, so a binary substituted later is still detected.  A build that pins
// nothing is protected against later substitution but not against a first
// download that was already wrong - which is exactly what the release pin fixes.
//
// The verification itself is a pure filesystem function so that it is tested on
// any platform (tests/core/DependencyPinTests.cpp) rather than only on Windows.
#pragma once

#include "core/Result.hpp"

#include <filesystem>
#include <string>

namespace monosect::core {

/// What a hash is a hash *of*.  The distinction is not cosmetic: the archive and
/// the extracted executables are separate files with separate digests, and each
/// must be checked against its own pin.
enum class PinSubject {
    Archive,      // the downloaded .zip
    Executable,   // ffmpeg.exe / ffprobe.exe
};

/// A release of a tool that MonoSect ships or downloads.
struct DependencyPin {
    std::string name;          // "ffmpeg archive" | "ffmpeg" | "ffprobe"
    std::string version;       // the release the product pins
    std::string url;           // download address (the archive pin only)
    std::string sha256;        // lower-case hex digest of `subject`
    PinSubject subject = PinSubject::Executable;

    /// True when this build states which artefact it expects.  An empty string
    /// means "not pinned"; anything else must be a valid SHA-256 - a malformed
    /// pin fails closed instead of falling back to trust-on-first-use.
    [[nodiscard]] bool hasPinnedHash() const noexcept { return !sha256.empty(); }
    /// The sidecar that records what was downloaded when nothing is pinned.
    [[nodiscard]] std::filesystem::path sidecarPath(const std::filesystem::path& toolPath) const;
    /// One sentence describing what this pin covers, for the log and the UI.
    [[nodiscard]] std::string describe() const;
};

/// The archive MonoSect downloads.  `sha256` is empty in a developer build.
[[nodiscard]] DependencyPin pinnedArchive();
/// The ffmpeg executable, verified after extraction.
[[nodiscard]] DependencyPin pinnedFfmpeg();
/// The ffprobe executable, verified after extraction.
[[nodiscard]] DependencyPin pinnedFfprobe();

enum class DependencyCheckOutcome {
    HashMatchesPin,        // the file is exactly the pinned release
    HashMismatchPin,       // the file is NOT the pinned release
    RecordedOnFirstUse,    // nothing was pinned; the hash of this file is now recorded
    HashMatchesRecord,     // the file matches what was downloaded/recorded before
    HashMismatchRecord,    // the file changed since it was recorded
    FileUnreadable,        // the file could not be read
    InvalidExpectedHash,   // a hash was supplied but is not 64 hex characters
};

[[nodiscard]] const char* dependencyCheckOutcomeName(DependencyCheckOutcome outcome) noexcept;

struct DependencyCheck {
    DependencyCheckOutcome outcome = DependencyCheckOutcome::FileUnreadable;
    std::string actualSha256;      // empty when the file could not be read
    std::string expectedSha256;    // the pin or the recorded hash, whichever applied
    std::string message;           // one sentence for the user, always filled in

    /// True when the file may be used.  A first-use recording counts as a pass
    /// because the file being recorded is the file that was just obtained.
    [[nodiscard]] bool ok() const noexcept {
        return outcome == DependencyCheckOutcome::HashMatchesPin ||
               outcome == DependencyCheckOutcome::RecordedOnFirstUse ||
               outcome == DependencyCheckOutcome::HashMatchesRecord;
    }
    /// True when the outcome is "this is not what we expected", i.e. the caller
    /// must discard the file.  `InvalidExpectedHash` is included: a build with a
    /// broken pin must not use the file.
    [[nodiscard]] bool mustDiscard() const noexcept {
        return outcome == DependencyCheckOutcome::HashMismatchPin ||
               outcome == DependencyCheckOutcome::HashMismatchRecord ||
               outcome == DependencyCheckOutcome::FileUnreadable ||
               outcome == DependencyCheckOutcome::InvalidExpectedHash;
    }
};

/// Hashes a file.  Returns an empty string when the file cannot be read.
[[nodiscard]] std::string sha256HexOfFile(const std::filesystem::path& path);

/// The verification described at the top of this file.  `recordWhenUnpinned`
/// controls whether an unpinned build writes the sidecar; the caller sets it to
/// false when it only wants to know, not to change anything on disk.
[[nodiscard]] DependencyCheck verifyToolFile(const std::filesystem::path& toolPath, const DependencyPin& pin,
                                            bool recordWhenUnpinned = true);

/// Removes the recorded hash, so a deliberate upgrade of a developer build does
/// not look like a substituted binary.
[[nodiscard]] bool forgetRecordedToolHash(const std::filesystem::path& toolPath, const DependencyPin& pin);

/// True when `text` is a 64-character lower/upper-case hexadecimal digest.
[[nodiscard]] bool isSha256Hex(std::string_view text);

} // namespace monosect::core
