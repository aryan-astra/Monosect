// MonoSect — transactional output.
//
// Parts are written into a job folder next to the output folder
// ("<output>\\.monosect-<job>\\"), verified there, and only then moved to their
// final name.  The move is the only moment at which a file becomes visible under
// its final name, and it never overwrites an existing file unless the user asked
// for that:
//   * Windows: MoveFileExW without MOVEFILE_REPLACE_EXISTING fails if the target
//     exists; the file is flushed first (FlushFileBuffers) so that a power loss
//     cannot leave a verified-looking but empty file behind.
//   * POSIX: link() + unlink() give the same "fails if the target exists"
//     behaviour, and are atomic within one file system.
// A cancel or a crash therefore leaves either nothing or a complete, verified
// file - never a half-written part with a final name.
#pragma once

#include "core/Bytes.hpp"
#include "core/Result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace monosect::core {

class OutputCommitter {
public:
    OutputCommitter(std::filesystem::path outputDirectory, std::string jobTag, bool overwriteExisting);

    /// Creates the job folder.  Must be called before anything else.
    [[nodiscard]] Status begin();

    [[nodiscard]] const std::filesystem::path& jobDirectory() const noexcept { return jobDirectory_; }

    /// Path inside the job folder that a part is written to first.
    [[nodiscard]] std::filesystem::path temporaryPathFor(const std::string& fileName) const;

    /// Flushes a written file to disk.  Called before the move so that the file
    /// that appears under the final name is complete on disk.
    [[nodiscard]] Status flushToDisk(const std::filesystem::path& file) const;

    /// Moves a verified file to its final name.  Fails (without touching the
    /// existing file) when the target exists and overwriting was not requested.
    [[nodiscard]] Status commit(const std::filesystem::path& temporaryFile,
                                const std::filesystem::path& finalFile);

    /// Deletes one temporary file.
    void discard(const std::filesystem::path& temporaryFile) const;

    /// Deletes the whole job folder.  Called on success, on failure and on cancel.
    void cleanUp() const;

    /// Removes leftover job folders from earlier runs.
    static int removeStaleJobDirectories(const std::filesystem::path& outputDirectory, int olderThanHours);

    /// Size of a file, or an error that names the path.
    [[nodiscard]] Result<ByteCount> fileSize(const std::filesystem::path& file) const;

    /// Free space on the volume that holds `directory` (0 when unknown).
    [[nodiscard]] ByteCount freeSpaceOnVolume(const std::filesystem::path& directory) const;

private:
    std::filesystem::path outputDirectory_;
    std::filesystem::path jobDirectory_;
    std::string jobTag_;
    bool overwriteExisting_ = false;
};

} // namespace monosect::core
