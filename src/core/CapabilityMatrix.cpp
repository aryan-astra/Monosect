#include "core/CapabilityMatrix.hpp"

#include <algorithm>
#include <sstream>

namespace monosect::core {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool matches(const CapabilityRow& row, const std::string& formatName) {
    const std::string needle = lower(formatName);
    for (const auto& candidate : row.ffprobeNames) {
        if (needle.find(candidate) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* capabilityStatusName(CapabilityStatus status) noexcept {
    switch (status) {
        case CapabilityStatus::Verified: return "Verified";
        case CapabilityStatus::Supported: return "Supported";
        case CapabilityStatus::Caveats: return "Caveats";
        case CapabilityStatus::Unsupported: return "Unsupported";
        case CapabilityStatus::Unknown: return "Not researched";
    }
    return "Not researched";
}

const std::vector<CapabilityRow>& CapabilityMatrix::rows() {
    // Status legend:
    //   Verified    - exercised by tests/e2e/run_e2e.sh for stream copy, smart cut,
    //                 size boundary, duration boundary, merge and verification.
    //   Supported   - implemented; needs Windows-side confirmation of the same
    //                 properties (documented in docs/LIMITATIONS.md).
    //   Caveats     - works with the restriction described in `notes`.
    static const std::vector<CapabilityRow> table{
        {"matroska", "Matroska / MKV", {"matroska", "webm"},
         CapabilityStatus::Verified, CapabilityStatus::Verified, CapabilityStatus::Verified,
         CapabilityStatus::Verified, CapabilityStatus::Verified,
         true, true,
         "Reference container for MonoSect: variable size timestamps, per stream time bases, chapters, "
         "attachments and subtitle tracks all survive a stream copy."},

        {"mp4", "MP4 / MOV / M4A / 3GP", {"mov,mp4,m4a,3gp,3g2,mj2", "mp4", "mov"},
         CapabilityStatus::Verified, CapabilityStatus::Verified, CapabilityStatus::Verified,
         CapabilityStatus::Verified, CapabilityStatus::Caveats,
         false, true,
         "Merge is verified for segments produced by MonoSect itself (identical codec parameters). Attachments "
         "cannot be carried by MP4; cover art survives as a video stream with the attached_pic flag."},

        {"mpegts", "MPEG-TS", {"mpegts", "mpeg-ts"},
         CapabilityStatus::Supported, CapabilityStatus::Supported, CapabilityStatus::Supported,
         CapabilityStatus::Supported, CapabilityStatus::Supported,
         false, false,
         "Timestamps are 90 kHz with PTS rounding error up to 1/90000 s. The planner keeps the rounding error "
         "inside the reported boundary error and never relies on bit-exact timestamps."},

        {"avi", "AVI", {"avi"},
         CapabilityStatus::Caveats, CapabilityStatus::Caveats, CapabilityStatus::Caveats,
         CapabilityStatus::Caveats, CapabilityStatus::Unsupported,
         false, false,
         "AVI has no global index and limited timestamp resolution. Size constrained splitting works; duration "
         "accuracy is limited to the frame grid, and merging mismatched AVI segments is refused."},

        {"flv", "FLV", {"flv"},
         CapabilityStatus::Caveats, CapabilityStatus::Caveats, CapabilityStatus::Unsupported,
         CapabilityStatus::Unsupported, CapabilityStatus::Unsupported,
         false, false,
         "FLV cannot store H.264 B-frames in a way that survives mid-GOP cuts. Only keyframe aligned cuts are "
         "offered, and no re-encoding."},

        {"webm", "WebM", {"webm"},
         CapabilityStatus::Verified, CapabilityStatus::Verified, CapabilityStatus::Verified,
         CapabilityStatus::Verified, CapabilityStatus::Verified,
         false, false,
         "WebM accepts only VP8/VP9/AV1 video and Vorbis/Opus audio. Copying an H.264 stream into WebM is "
         "refused by the planner with an explanation instead of failing at mux time."},
    };
    return table;
}

std::optional<CapabilityRow> CapabilityMatrix::lookupByProbeName(const std::string& formatName) {
    for (const auto& row : rows()) {
        if (matches(row, formatName)) {
            return row;
        }
    }
    return std::nullopt;
}

bool CapabilityMatrix::supportsStreamCopy(const std::string& formatName) {
    const auto row = lookupByProbeName(formatName);
    if (!row.has_value()) {
        return false;
    }
    return row->streamCopy == CapabilityStatus::Verified || row->streamCopy == CapabilityStatus::Supported ||
           row->streamCopy == CapabilityStatus::Caveats;
}

std::string CapabilityMatrix::renderTable() {
    std::ostringstream out;
    out << "| Format | Input | Stream copy | Exact cut | Smart cut | Merge | Attachments | Chapters |\n";
    out << "|---|---|---|---|---|---|---|---|\n";
    for (const auto& row : rows()) {
        out << "| " << row.displayName << " | " << capabilityStatusName(row.input) << " | "
            << capabilityStatusName(row.streamCopy) << " | " << capabilityStatusName(row.exactCut) << " | "
            << capabilityStatusName(row.smartCut) << " | " << capabilityStatusName(row.merge) << " | "
            << (row.allowsAttachments ? "yes" : "no") << " | " << (row.allowsChapters ? "yes" : "no")
            << " |\n";
    }
    return out.str();
}

} // namespace monosect::core
