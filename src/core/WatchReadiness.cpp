#include "core/WatchReadiness.hpp"

#include "core/CapabilityMatrix.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace monosect::core {
namespace {

std::string lowerExtension(std::string_view path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t separator = path.find_last_of("/\\");
    if (dot == std::string_view::npos) {
        return {};
    }
    if (separator != std::string_view::npos && dot < separator) {
        return {};   // a dot in a directory name, not in the file name
    }
    std::string extension(path.substr(dot + 1));
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

std::string formatReason(const std::string& text) {
    std::ostringstream out;
    out << text;
    return out.str();
}

} // namespace

const std::vector<std::string>& watchableMediaExtensions() {
    // Every container family in the capability matrix that MonoSect can read,
    // plus the raw elementary streams FFmpeg can demux on its own.  The list is
    // derived from the matrix rather than maintained separately, so a format
    // that is added to the matrix cannot be forgotten here.
    static const std::vector<std::string> extensions = [] {
        std::vector<std::string> values{"mkv", "mka", "webm", "mp4", "m4v", "mov", "m4a", "m4b", "3gp", "ts",
                                         "m2ts", "mts", "avi", "flv", "f4v", "ogv", "ogg", "oga", "wav", "mp3",
                                         "aac", "ac3", "eac3", "flac", "opus", "wmv", "asf", "rm", "rmvb", "vob",
                                         "dv", "gxf", "mxf", "y4m", "gif", "h264", "h265", "hevc", "ivf"};
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }();
    return extensions;
}

bool isWatchableMediaExtension(const std::filesystem::path& path) {
    const std::string extension = lowerExtension(path.string());
    if (extension.empty()) {
        return false;
    }
    const std::vector<std::string>& all = watchableMediaExtensions();
    return std::binary_search(all.begin(), all.end(), extension);
}

ReadinessDecision decideReadiness(const WatchCandidate& candidate, ReadinessHistory& history,
                                  std::uint64_t nowTicks, const ReadinessSettings& settings) {
    ReadinessDecision decision;

    if (!isWatchableMediaExtension(candidate.path)) {
        decision.verdict = ReadinessVerdict::Rejected;
        std::ostringstream reason;
        reason << "not a media file MonoSect handles (extension of '" << candidate.path.filename().string() << "')";
        decision.reason = reason.str();
        return decision;
    }

    if (!history.seen) {
        history.seen = true;
        history.lastSize = candidate.size;
        history.lastSizeChangeTicks = nowTicks;
        history.firstSeenTicks = nowTicks;
    } else if (candidate.size != history.lastSize) {
        history.lastSize = candidate.size;
        history.lastSizeChangeTicks = nowTicks;
    }

    if (history.announced) {
        decision.verdict = ReadinessVerdict::NotYet;
        decision.reason = "already announced";
        return decision;
    }

    if (candidate.size == 0) {
        decision.reason = "the file is still empty";
        return decision;
    }
    if (candidate.size < settings.minimumBytes) {
        std::ostringstream reason;
        reason << "the file is only " << candidate.size << " bytes so far, below the "
               << settings.minimumBytes << " byte floor";
        decision.reason = reason.str();
        return decision;
    }
    if (!candidate.exclusiveOpenSucceeded) {
        // The important one: this is the signal that a producer still holds the
        // file open.  A size that has not changed is not evidence of that; an
        // open handle is.
        std::ostringstream reason;
        reason << "another program still has the file open"
               << (candidate.openFailureReason.empty() ? "" : " (" + candidate.openFailureReason + ")");
        decision.reason = reason.str();
        return decision;
    }
    if (!candidate.readable) {
        decision.reason = "the file could not be read even though nothing holds it open";
        return decision;
    }

    const std::uint64_t settledFor =
        nowTicks > history.lastSizeChangeTicks ? nowTicks - history.lastSizeChangeTicks : 0;
    const std::uint64_t requiredMillis = static_cast<std::uint64_t>(settings.settlingSeconds * 1000.0);
    if (settledFor < requiredMillis) {
        std::ostringstream reason;
        reason << "the size has been unchanged for " << settledFor << " ms of the " << requiredMillis
               << " ms settling interval";
        decision.reason = reason.str();
        return decision;
    }

    history.announced = true;
    decision.verdict = ReadinessVerdict::Ready;
    decision.newlyReady = true;
    std::ostringstream reason;
    reason << "nothing has the file open, it is readable, and its size has been unchanged for " << settledFor
           << " ms";
    decision.reason = reason.str();
    return decision;
}

} // namespace monosect::core
