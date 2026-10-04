// MonoSect — the ffprobe front end.
//
// Exactly one class talks to ffprobe.  It runs the two documented commands
// (container probe and packet index), parses them with the core parser, caches
// the result for the lifetime of a job (keyed by path + size + write time, so a
// file that changes on disk is probed again), and turns failures into errors
// that say what to do about them.
#pragma once

#include "core/ProbeParsing.hpp"
#include "engine/ProcessRunner.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace monosect::core {

struct ProbeCacheStats {
    int containerProbes = 0;
    int packetProbes = 0;
    int cacheHits = 0;
    double probeSeconds = 0.0;
};

class FFprobeService {
public:
    FFprobeService(IProcessRunner& runner, std::string ffprobePath);

    /// Container probe (streams, format, chapters).  Used for the source and for
    /// every written part.
    [[nodiscard]] Result<MediaModel> probe(const std::filesystem::path& path);

    /// Packet index for a file.  Requires timestamps, so it is only run on files
    /// the container probe already accepted.
    [[nodiscard]] Result<PacketIndex> packetIndex(const std::filesystem::path& path);

    /// Convenience: probe + index in one call.
    [[nodiscard]] Result<ProbeSnapshot> probeAll(const std::filesystem::path& path);

    /// `ffprobe -version`, used to record the exact tool version in the job log.
    [[nodiscard]] Result<std::string> version();

    /// Drops every cached result.  Called when a job starts so that a file
    /// changed behind MonoSect's back is seen.
    void clearCache();

    [[nodiscard]] const ProbeCacheStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string& path() const noexcept { return ffprobePath_; }

    /// Turns ffprobe stderr into a user-facing error.
    [[nodiscard]] static Error interpretProbeFailure(const ProcessResult& result, const std::filesystem::path& path);

private:
    struct CacheKey {
        std::string path;
        std::uintmax_t size = 0;
        std::int64_t writeTimeTicks = 0;

        [[nodiscard]] bool operator<(const CacheKey& other) const;
    };

    struct CacheEntry {
        MediaModel model;
        std::optional<PacketIndex> index;
    };

    [[nodiscard]] Result<CacheKey> keyFor(const std::filesystem::path& path) const;

    IProcessRunner& runner_;
    std::string ffprobePath_;
    std::map<CacheKey, CacheEntry> cache_;
    ProbeCacheStats stats_;
};

} // namespace monosect::core
