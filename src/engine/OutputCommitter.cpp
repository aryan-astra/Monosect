#include "engine/OutputCommitter.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace monosect::core {
namespace {

std::string randomSuffix() {
    std::random_device device;
    std::mt19937_64 generator(device());
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%06llx", static_cast<unsigned long long>(generator() & 0xFFFFFFull));
    return buffer;
}

std::string sanitizeTag(const std::string& tag) {
    std::string out;
    for (const char c : tag) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(c);
        }
    }
    if (out.empty()) {
        out = "job";
    }
    if (out.size() > 24) {
        out.resize(24);
    }
    return out;
}

} // namespace

OutputCommitter::OutputCommitter(std::filesystem::path outputDirectory, std::string jobTag, bool overwriteExisting)
    : outputDirectory_(std::move(outputDirectory)), jobTag_(sanitizeTag(jobTag)),
      overwriteExisting_(overwriteExisting) {}

Status OutputCommitter::begin() {
    std::error_code error;
    std::filesystem::create_directories(outputDirectory_, error);
    if (error) {
        return makeError(ErrorCode::OutputWriteFailed, "The output folder could not be created.",
                         outputDirectory_.string() + ": " + error.message(),
                         "Choose a folder you can write to.");
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        jobDirectory_ = outputDirectory_ / (".monosect-" + jobTag_ + "-" + randomSuffix());
        std::error_code createError;
        if (std::filesystem::create_directory(jobDirectory_, createError)) {
            return Status::success();
        }
        if (createError && createError != std::errc::file_exists) {
            return makeError(ErrorCode::OutputWriteFailed, "The working folder for this job could not be created.",
                             jobDirectory_.string() + ": " + createError.message(),
                             "Check the permissions of the output folder.");
        }
    }
    return makeError(ErrorCode::OutputWriteFailed, "The working folder for this job could not be created.",
                     "eight attempts, all collided", "Try again, or choose a different output folder.");
}

std::filesystem::path OutputCommitter::temporaryPathFor(const std::string& fileName) const {
    return jobDirectory_ / (fileName + ".partial");
}

Status OutputCommitter::flushToDisk(const std::filesystem::path& file) const {
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return makeError(ErrorCode::OutputWriteFailed, "A written part could not be opened for flushing.",
                         file.string(), "Check that the drive is not full.");
    }
    const BOOL flushed = ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
    if (!flushed) {
        return makeError(ErrorCode::OutputWriteFailed, "A written part could not be flushed to disk.",
                         file.string(), "Check the drive for errors.");
    }
    return Status::success();
#else
    const int descriptor = ::open(file.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return makeError(ErrorCode::OutputWriteFailed, "A written part could not be opened for flushing.",
                         file.string(), "Check that the drive is not full.");
    }
    const int result = ::fsync(descriptor);
    ::close(descriptor);
    if (result != 0) {
        return makeError(ErrorCode::OutputWriteFailed, "A written part could not be flushed to disk.",
                         file.string(), "Check the drive for errors.");
    }
    return Status::success();
#endif
}

Status OutputCommitter::commit(const std::filesystem::path& temporaryFile,
                               const std::filesystem::path& finalFile) {
    std::error_code error;
    if (!std::filesystem::exists(temporaryFile, error) || error) {
        return makeError(ErrorCode::OutputWriteFailed, "The verified part is missing and cannot be published.",
                         temporaryFile.string(), "This is a defect; please report it.");
    }
    if (std::filesystem::exists(finalFile, error) && !error && !overwriteExisting_) {
        return makeError(ErrorCode::OutputCollisionUnresolved,
                         "A file with the target name appeared while the job was running.",
                         finalFile.string(),
                         "MonoSect will not overwrite it; the part is written again under a free name.");
    }

#if defined(_WIN32)
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (overwriteExisting_) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!::MoveFileExW(temporaryFile.wstring().c_str(), finalFile.wstring().c_str(), flags)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            return makeError(ErrorCode::OutputCollisionUnresolved, "The target file already exists.",
                             finalFile.string(), "MonoSect will not overwrite it.");
        }
        return makeError(ErrorCode::OutputWriteFailed, "The verified part could not be moved to its final name.",
                         finalFile.string() + ", Windows error " + std::to_string(code),
                         "Check that the output folder is writable and not on a disconnected drive.");
    }
    return Status::success();
#else
    if (overwriteExisting_) {
        std::filesystem::rename(temporaryFile, finalFile, error);
        if (error) {
            return makeError(ErrorCode::OutputWriteFailed, "The verified part could not be moved to its final name.",
                             finalFile.string() + ": " + error.message(), "Check the output folder.");
        }
        return Status::success();
    }
    // link() fails with EEXIST when the target is already there, which is exactly
    // the "never overwrite" behaviour that is wanted; both paths are inside the
    // same directory, so they are on the same file system.
    if (::link(temporaryFile.c_str(), finalFile.c_str()) != 0) {
        if (errno == EEXIST) {
            return makeError(ErrorCode::OutputCollisionUnresolved, "The target file already exists.",
                             finalFile.string(), "MonoSect will not overwrite it.");
        }
        return makeError(ErrorCode::OutputWriteFailed, "The verified part could not be moved to its final name.",
                         finalFile.string() + ": " + std::strerror(errno), "Check the output folder.");
    }
    ::unlink(temporaryFile.c_str());
    return Status::success();
#endif
}

void OutputCommitter::discard(const std::filesystem::path& temporaryFile) const {
    std::error_code error;
    std::filesystem::remove(temporaryFile, error);
}

void OutputCommitter::cleanUp() const {
    std::error_code error;
    std::filesystem::remove_all(jobDirectory_, error);
}

int OutputCommitter::removeStaleJobDirectories(const std::filesystem::path& outputDirectory, int olderThanHours) {
    std::error_code error;
    if (!std::filesystem::exists(outputDirectory, error)) {
        return 0;
    }
    int removed = 0;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(outputDirectory, error)) {
        if (error) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(".monosect-", 0) != 0) {
            continue;
        }
        if (!entry.is_directory(error)) {
            continue;
        }
        const auto writeTime = std::filesystem::last_write_time(entry.path(), error);
        if (error) {
            continue;
        }
        const auto age = std::chrono::duration_cast<std::chrono::hours>(now - writeTime).count();
        if (age >= olderThanHours) {
            std::error_code removalError;
            std::filesystem::remove_all(entry.path(), removalError);
            if (!removalError) {
                ++removed;
            }
        }
    }
    return removed;
}

Result<ByteCount> OutputCommitter::fileSize(const std::filesystem::path& file) const {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error) {
        return Error{ErrorCode::FileNotReadable, "A file's size could not be read.",
                     file.string() + ": " + error.message(), "Check that the file still exists."};
    }
    return static_cast<ByteCount>(size);
}

ByteCount OutputCommitter::freeSpaceOnVolume(const std::filesystem::path& directory) const {
    std::error_code error;
    const std::filesystem::space_info info = std::filesystem::space(directory, error);
    if (error) {
        return 0;
    }
    return static_cast<ByteCount>(info.available);
}

} // namespace monosect::core
