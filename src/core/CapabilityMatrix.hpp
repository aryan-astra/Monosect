// MonoSect — container capability matrix (TODO 107 - TODO 112).
//
// The matrix is intentionally conservative and *verifiable*: every row carries
// the evidence level it was established at. Nothing is advertised as supported
// because "FFmpeg probably handles it" - a row is only `Verified` when the
// automated fixture suite (tests/harness/end_to_end.py) covers it, and the row's
// own `notes` field says what the coverage does not include.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

enum class CapabilityStatus {
    Verified,          // covered by the automated media fixture suite
    Supported,         // implemented and manually validated, not yet in the fixture suite
    Caveats,           // works with documented restrictions
    Unsupported,       // refused by the planner with an explanation
    Unknown,           // not researched yet - never advertised in the UI
};

[[nodiscard]] const char* capabilityStatusName(CapabilityStatus status) noexcept;

struct CapabilityRow {
    std::string formatId;       // canonical id used by the planner, e.g. "matroska"
    std::string displayName;    // "Matroska / MKV"
    std::vector<std::string> ffprobeNames; // format_name fragments that map here
    CapabilityStatus input = CapabilityStatus::Unknown;
    CapabilityStatus streamCopy = CapabilityStatus::Unknown;
    CapabilityStatus exactCut = CapabilityStatus::Unknown;
    CapabilityStatus smartCut = CapabilityStatus::Unknown;
    CapabilityStatus merge = CapabilityStatus::Unknown;
    bool allowsAttachments = false;
    bool allowsChapters = false;
    std::string notes;
};

class CapabilityMatrix {
public:
    [[nodiscard]] static const std::vector<CapabilityRow>& rows();

    // Matches the ffprobe `format_name` list ("mov,mp4,m4a,3gp,3g2,mj2").
    [[nodiscard]] static std::optional<CapabilityRow> lookupByProbeName(const std::string& formatName);

    // True when the planner may stream-copy into this container.
    [[nodiscard]] static bool supportsStreamCopy(const std::string& formatName);

    [[nodiscard]] static std::string renderTable();
};

} // namespace monosect::core
