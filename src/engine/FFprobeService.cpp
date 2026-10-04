#include "engine/FFprobeService.hpp"

#include <chrono>
#include <system_error>

namespace monosect::core {

bool FFprobeService::CacheKey::operator<(const CacheKey& other) const {
    if (path != other.path) {
        return path < other.path;
    }
    if (size != other.size) {
        return size < other.size;
    }
    return writeTimeTicks < other.writeTimeTicks;
}

FFprobeService::FFprobeService(IProcessRunner& runner, std::string ffprobePath)
    : runner_(runner), ffprobePath_(std::move(ffprobePath)) {}

Result<FFprobeService::CacheKey> FFprobeService::keyFor(const std::filesystem::path& path) const {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return Error{ErrorCode::FileNotFound, "That file does not exist (or cannot be reached).",
                     "path: " + path.string(), "Check the path and try again."};
    }
    CacheKey key;
    key.path = path.string();
    key.size = std::filesystem::file_size(path, error);
    if (error) {
        key.size = 0;
    }
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (!error) {
        key.writeTimeTicks = static_cast<std::int64_t>(writeTime.time_since_epoch().count());
    }
    return key;
}

Error FFprobeService::interpretProbeFailure(const ProcessResult& result, const std::filesystem::path& path) {
    if (!result.launched) {
        return Error{ErrorCode::ProbeBinaryMissing,
                     "ffprobe could not be started.",
                     result.launchError.empty() ? std::string() : result.launchError,
                     "Install the FFmpeg runtime from Settings, or point MonoSect at an existing ffprobe.exe."};
    }
    std::string detail;
    for (const std::string& line : result.stderrTail) {
        if (detail.size() > 600) {
            break;
        }
        detail += line;
        detail += ' ';
    }
    if (detail.empty()) {
        detail = "ffprobe exited with code " + std::to_string(result.exitCode);
    }
    if (detail.find("No such file or directory") != std::string::npos) {
        return Error{ErrorCode::FileNotFound, "That file could not be opened.", detail,
                     "Check that the file still exists and that no other program has locked it."};
    }
    if (detail.find("Permission denied") != std::string::npos) {
        return Error{ErrorCode::FileAccessDenied, "That file could not be opened because access was denied.", detail,
                     "Close the program that is holding the file, or copy it to a folder you own."};
    }
    if (detail.find("Invalid data found") != std::string::npos ||
        detail.find("moov atom not found") != std::string::npos ||
        detail.find("End of file") != std::string::npos) {
        return Error{ErrorCode::UnsupportedInputFormat,
                     "That file is not a complete media file MonoSect can read.",
                     detail + " path: " + path.string(),
                     "Check that the file is not still being downloaded or copied."};
    }
    return Error{ErrorCode::ProbeFailed, "Reading the file's structure failed.", detail,
                 "The file may be damaged; try another file to confirm."};
}

Result<std::string> FFprobeService::version() {
    ProcessSpec spec;
    spec.program = ffprobePath_;
    spec.arguments = {"-hide_banner", "-v", "error", "-version"};
    spec.captureStdout = true;
    spec.captureStderr = true;
    const ProcessResult result = runner_.run(spec, ProcessCallbacks{});
    if (!result.launched || result.exitCode != 0) {
        return failure<std::string>(interpretProbeFailure(result, {}));
    }
    const std::vector<std::string> lines = splitLines(result.stdoutText);
    if (lines.empty()) {
        return Error{ErrorCode::ProbeBinaryMissing, "ffprobe did not report its version.", ffprobePath_,
                     "Reinstall the FFmpeg runtime from Settings."};
    }
    return lines.front();
}

Result<MediaModel> FFprobeService::probe(const std::filesystem::path& path) {
    auto key = keyFor(path);
    if (!key.has_value()) {
        return failure<MediaModel>(key.error());
    }
    if (const auto found = cache_.find(key.value()); found != cache_.end()) {
        ++stats_.cacheHits;
        return found->second.model;
    }

    ProcessSpec spec;
    spec.program = ffprobePath_;
    spec.arguments = probeArguments(path.string(), /*wantChapters=*/true);
    spec.captureStdout = true;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = runner_.run(spec, ProcessCallbacks{});
    stats_.probeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ++stats_.containerProbes;
    if (!result.launched || result.exitCode != 0) {
        return failure<MediaModel>(interpretProbeFailure(result, path));
    }

    auto document = parseJson(result.stdoutText);
    if (!document.has_value()) {
        return failure<MediaModel>(document.error());
    }

    std::error_code error;
    const ByteCount size = static_cast<ByteCount>(std::filesystem::file_size(path, error));
    auto model = parseContainerModel(document.value(), path.string(), error ? 0 : size);
    if (!model.has_value()) {
        return model;
    }
    cache_[key.value()] = CacheEntry{model.value(), std::nullopt};
    return model;
}

Result<PacketIndex> FFprobeService::packetIndex(const std::filesystem::path& path) {
    auto key = keyFor(path);
    if (!key.has_value()) {
        return failure<PacketIndex>(key.error());
    }
    if (const auto found = cache_.find(key.value());
        found != cache_.end() && found->second.index.has_value()) {
        ++stats_.cacheHits;
        return *found->second.index;
    }

    MediaModel model;
    if (const auto found = cache_.find(key.value()); found != cache_.end()) {
        model = found->second.model;
    } else {
        auto probed = probe(path);
        if (!probed.has_value()) {
            return failure<PacketIndex>(probed.error());
        }
        model = probed.value();
    }

    ProcessSpec spec;
    spec.program = ffprobePath_;
    spec.arguments = packetIndexArguments(path.string());
    spec.captureStdout = true;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = runner_.run(spec, ProcessCallbacks{});
    stats_.probeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ++stats_.packetProbes;
    if (!result.launched || result.exitCode != 0) {
        return failure<PacketIndex>(interpretProbeFailure(result, path));
    }

    auto document = parseJson(result.stdoutText);
    if (!document.has_value()) {
        return failure<PacketIndex>(document.error());
    }
    std::size_t withoutTimestamp = 0;
    auto index = parsePacketSection(document.value(), model, &withoutTimestamp);
    if (!index.has_value()) {
        return index;
    }
    if (index.value().totalPacketCount() == 0) {
        return Error{ErrorCode::ProbeFailed,
                     "ffprobe reported no packets for this file, so its exact sizes cannot be planned.",
                     "path: " + path.string(),
                     "The file may use a format MonoSect cannot index packet by packet; try re-muxing it to MKV or "
                     "MP4 first."};
    }
    const std::size_t untimed = index.value().untimedPacketCount();
    if (untimed > 0) {
        // Recorded in the model's warnings so the plan can say it.
        model.warnings.push_back(std::to_string(untimed) +
                                 " packet(s) carry no usable timestamp; they are counted in the file size but not "
                                 "in any time range");
    }

    // A still image is recognised twice, and the second test is the one that works
    // in practice.  ProbeParsing sets the flag from nb_frames == 1, but the
    // containers that actually carry single images - a bare PNG is a piped image
    // sequence, and so reports nb_frames as N/A - do not report a frame count at
    // all.  The packet index is always built, so it is the reliable evidence: a
    // file with exactly one video packet and nothing else has no timeline to split
    // along.  Verified on this machine: a 799 byte PNG reports nb_frames = N/A,
    // one video packet, and duration N/A, and MonoSect planned it instead of
    // refusing with "This file contains a single image and cannot be split into
    // time ranges".
    if (!model.isStillImage && index.value().totalPacketCount() == 1 && model.streams.size() == 1 &&
        model.streams.front().kind == StreamKind::Video) {
        model.isStillImage = true;
    }

    auto& entry = cache_[key.value()];
    entry.model = model;
    entry.index = index.value();
    return index;
}

Result<ProbeSnapshot> FFprobeService::probeAll(const std::filesystem::path& path) {
    ProbeSnapshot snapshot;
    auto index = packetIndex(path);
    if (!index.has_value()) {
        return failure<ProbeSnapshot>(index.error());
    }
    // Read the model back out of the cache rather than calling probe() first:
    // packetIndex() re-probes and may have refined it - it is what recognises a
    // still image from the packet count - and reading the cached copy is what makes
    // that refinement visible to the caller.  Calling probe() here instead returned
    // the pre-refinement model, so the caller planned a file it had already been
    // told it must refuse.
    const auto key = keyFor(path);
    if (!key.has_value()) {
        return failure<ProbeSnapshot>(key.error());
    }
    const auto found = cache_.find(key.value());
    if (found == cache_.end()) {
        return Error{ErrorCode::ProbeFailed, "The probe result went missing while it was being read.",
                     "path: " + path.string(), "Try again."};
    }
    snapshot.container = found->second.model;
    snapshot.packets = index.value();
    snapshot.hasPacketIndex = true;
    snapshot.packetsWithoutTimestamp = index.value().untimedPacketCount();
    snapshot.packetsRejected = 0;
    return snapshot;
}

void FFprobeService::clearCache() {
    cache_.clear();
}

} // namespace monosect::core
