#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace vsm {

// Ensures ffmpeg.exe and ffprobe.exe exist next to the application executable.
// If missing, downloads FFmpeg essentials (~30 MB) from the official gyan.dev release
// and extracts the binaries. Returns true on success.
//
// The `onStatus` callback is invoked with progress messages suitable for UI display.
[[nodiscard]] bool ensureFFmpeg(
    const std::filesystem::path& appDirectory,
    const std::function<void(const std::string&)>& onStatus = nullptr);

}
