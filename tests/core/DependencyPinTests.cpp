// MonoSect — the tools MonoSect runs are identified, not trusted (TODO 221, 315).
//
// The bootstrap downloads ffmpeg.exe and ffprobe.exe.  These tests exercise the
// verification of those files with real files on disk, on any platform: a pinned
// hash must match, a mismatch must be refused, and a build with no pinned hash
// must record what it obtained and enforce it afterwards.
//
// The archive/executable distinction is tested explicitly, because treating the
// two as one thing is how a "verified" toolchain ends up deleting the tools it
// just verified: one digest cannot match both a 200 MB zip and the 90 MB
// executable inside it.
#include "TestSupport.hpp"

#include "core/DependencyPin.hpp"
#include "core/Sha256.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace monosect::core;
using namespace monosect::test;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        // A unique folder per instance, without relying on a platform-specific
        // process id (the tests build on Windows and on POSIX).
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("monosect-pin-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

DependencyPin pinWith(const std::string& hash, PinSubject subject = PinSubject::Executable) {
    DependencyPin pin;
    pin.name = subject == PinSubject::Archive ? "the FFmpeg archive" : "ffmpeg.exe";
    pin.version = "test";
    pin.url = "https://example.invalid/ffmpeg.zip";
    pin.sha256 = hash;
    pin.subject = subject;
    return pin;
}

} // namespace

MONOSECT_TEST(the_file_hash_is_the_sha256_of_the_bytes) {
    const TempDir directory;
    const std::filesystem::path file = directory.path / "tool.bin";
    writeFile(file, "abc");
    // FIPS 180-4 test vector: SHA-256("abc").
    CHECK_EQ(std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
             sha256HexOfFile(file));
    CHECK(!std::filesystem::exists(std::filesystem::path(file.string() + ".sha256")));

    // A file that cannot be read has no hash - and no hash is not a pass.
    CHECK(sha256HexOfFile(directory.path / "missing.bin").empty());

    // isSha256Hex is the gate the runtime uses, and it must reject near misses.
    CHECK(isSha256Hex(std::string(64, 'a')));
    CHECK(isSha256Hex(std::string(64, 'F')));
    CHECK(!isSha256Hex(std::string(63, 'a')));
    CHECK(!isSha256Hex(std::string(64, 'a') + "0"));
    CHECK(!isSha256Hex(std::string(64, 'z')));
}

MONOSECT_TEST(a_pinned_hash_is_required_to_match) {
    const TempDir directory;
    const std::filesystem::path file = directory.path / "ffmpeg.exe";
    writeFile(file, "abc");
    const std::string correct = sha256Hex("abc");

    DependencyCheck check = verifyToolFile(file, pinWith(correct));
    CHECK(check.ok());
    CHECK(!check.mustDiscard());
    CHECK(check.outcome == DependencyCheckOutcome::HashMatchesPin);
    CHECK_EQ(correct, check.expectedSha256);
    CHECK(check.message.find("pinned release") != std::string::npos);

    // A different file is refused, and the message says both hashes.
    writeFile(file, "not the pinned build");
    check = verifyToolFile(file, pinWith(correct));
    CHECK(!check.ok());
    CHECK(check.mustDiscard());
    CHECK(check.outcome == DependencyCheckOutcome::HashMismatchPin);
    CHECK(check.message.find(correct) != std::string::npos);
    CHECK(check.message.find(sha256Hex("not the pinned build")) != std::string::npos);

    // A hash that is not a hash is refused rather than compared as a string.
    check = verifyToolFile(file, pinWith("deadbeef"));
    CHECK(!check.ok());
    CHECK(check.mustDiscard());
    CHECK(check.outcome == DependencyCheckOutcome::InvalidExpectedHash);

    // A deleted file cannot be verified.
    std::filesystem::remove(file);
    check = verifyToolFile(file, pinWith(correct));
    CHECK(!check.ok());
    CHECK(check.mustDiscard());
    CHECK(check.outcome == DependencyCheckOutcome::FileUnreadable);
}

MONOSECT_TEST(without_a_pinned_hash_the_first_download_is_recorded_and_then_enforced) {
    const TempDir directory;
    const std::filesystem::path file = directory.path / "ffmpeg.exe";
    writeFile(file, "downloaded build");
    const DependencyPin pin = pinWith("");   // a developer build: nothing pinned

    DependencyCheck check = verifyToolFile(file, pin);
    CHECK(check.ok());   // the file being recorded is the file just obtained
    CHECK(!check.mustDiscard());
    CHECK(check.outcome == DependencyCheckOutcome::RecordedOnFirstUse);
    CHECK(std::filesystem::exists(pin.sidecarPath(file)));
    CHECK_EQ(64u, check.actualSha256.size());

    // The same file passes from then on.
    check = verifyToolFile(file, pin);
    CHECK(check.ok());
    CHECK(check.outcome == DependencyCheckOutcome::HashMatchesRecord);

    // A binary that appears in the same place later does not pass.
    writeFile(file, "a different binary");
    check = verifyToolFile(file, pin);
    CHECK(!check.ok());
    CHECK(check.mustDiscard());
    CHECK(check.outcome == DependencyCheckOutcome::HashMismatchRecord);
    CHECK(check.message.find("differs from the one MonoSect obtained earlier") != std::string::npos);

    // Forgetting the record is an explicit action, not a silent fallback.
    CHECK(forgetRecordedToolHash(file, pin));
    CHECK(!std::filesystem::exists(pin.sidecarPath(file)));
    writeFile(file, "a newer build");
    check = verifyToolFile(file, pin);
    CHECK(check.ok());
    CHECK(check.outcome == DependencyCheckOutcome::RecordedOnFirstUse);
}

MONOSECT_TEST(a_check_that_must_not_write_leaves_the_disk_alone) {
    // The bootstrap asks "is this file what I expect?" while it is still deciding
    // whether to install anything.  That question must not have the side effect of
    // recording a hash for a file that is then thrown away.
    const TempDir directory;
    const std::filesystem::path file = directory.path / "staged-ffmpeg.exe";
    writeFile(file, "staged");
    const DependencyPin pin = pinWith("");
    const DependencyCheck check = verifyToolFile(file, pin, /*recordWhenUnpinned=*/false);
    CHECK(check.ok());
    CHECK(check.outcome == DependencyCheckOutcome::HashMatchesRecord);
    CHECK(!std::filesystem::exists(pin.sidecarPath(file)));
}

MONOSECT_TEST(the_archive_and_the_executables_are_separate_pins) {
    const TempDir directory;
    const std::filesystem::path archive = directory.path / "ffmpeg.zip";
    const std::filesystem::path ffmpeg = directory.path / "ffmpeg.exe";
    const std::filesystem::path ffprobe = directory.path / "ffprobe.exe";
    writeFile(archive, "a compressed archive");
    writeFile(ffmpeg, "an executable");
    writeFile(ffprobe, "another executable");

    // The digests are genuinely different, which is the whole point.
    const std::string archiveHash = sha256Hex("a compressed archive");
    const std::string ffmpegHash = sha256Hex("an executable");
    const std::string ffprobeHash = sha256Hex("another executable");
    CHECK(archiveHash != ffmpegHash);
    CHECK(ffmpegHash != ffprobeHash);

    // Each file passes against its own pin...
    CHECK(verifyToolFile(archive, pinWith(archiveHash, PinSubject::Archive)).ok());
    CHECK(verifyToolFile(ffmpeg, pinWith(ffmpegHash, PinSubject::Executable)).ok());
    CHECK(verifyToolFile(ffprobe, pinWith(ffprobeHash, PinSubject::Executable)).ok());

    // ...and fails against the wrong one.  Checking an executable against the
    // archive digest is exactly the mistake this separation exists to prevent: it
    // can never succeed, so a toolchain verified that way would delete itself.
    const DependencyCheck confused =
        verifyToolFile(ffmpeg, pinWith(archiveHash, PinSubject::Archive));
    CHECK(!confused.ok());
    CHECK(confused.outcome == DependencyCheckOutcome::HashMismatchPin);
    CHECK(confused.mustDiscard());

    const DependencyPin unlabelled = pinWith(archiveHash, PinSubject::Archive);
    CHECK(unlabelled.describe().find("archive") != std::string::npos);
    const DependencyPin tool = pinWith(ffmpegHash, PinSubject::Executable);
    CHECK(tool.describe().find("executable") != std::string::npos);
    CHECK(tool.describe().find("pinned") != std::string::npos);
}

MONOSECT_TEST(the_shipped_pins_name_a_source_a_version_and_a_real_address) {
    const DependencyPin archive = pinnedArchive();
    const DependencyPin ffmpeg = pinnedFfmpeg();
    const DependencyPin ffprobe = pinnedFfprobe();

    // The archive is the download address; the executables are not downloads.
    CHECK_EQ(PinSubject::Archive, archive.subject);
    CHECK(archive.url.rfind("https://", 0) == 0);
    CHECK(archive.url.find("/releases/download/latest/") == std::string::npos);
    CHECK(archive.url.find("/download/latest/") == std::string::npos);
    CHECK(!archive.version.empty());

    CHECK_EQ(PinSubject::Executable, ffmpeg.subject);
    CHECK(ffmpeg.url.empty());
    CHECK_EQ(std::string("ffmpeg.exe"), ffmpeg.name);
    CHECK_EQ(std::string("ffprobe.exe"), ffprobe.name);
    CHECK_EQ(archive.version, ffmpeg.version);
    CHECK_EQ(archive.version, ffprobe.version);

    // Every pinned digest is a real digest: a "pinned" build with a malformed pin
    // would fail identically for every user, so it must be caught as a build
    // error (CMake does) and again here.
    for (const DependencyPin* pin : {&archive, &ffmpeg, &ffprobe}) {
        if (pin->hasPinnedHash()) {
            CHECK(isSha256Hex(pin->sha256));
        }
    }
    // The two executable digests, when both are pinned, must differ: two
    // different files cannot share one digest.
    if (ffmpeg.hasPinnedHash() && ffprobe.hasPinnedHash()) {
        CHECK(ffmpeg.sha256 != ffprobe.sha256);
    }
}
