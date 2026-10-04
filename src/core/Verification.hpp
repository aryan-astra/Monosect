// MonoSect — verification rules.
//
// Verification is a separate step that runs after a part has been written and
// before it is published.  It answers three questions:
//   1. Did the part land inside the hard limits? (size in bytes, duration)
//   2. Does it still contain the streams the source has, with the same codec
//      parameters? (stream signature comparison)
//   3. As a set, do the parts cover the source without gaps or overlaps?
//
// A result that fails an error-level finding must never be reported as success
// and must never be committed to its final file name.
#pragma once

#include "core/MediaModel.hpp"
#include "core/SplitPlan.hpp"

#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

enum class FindingSeverity { Info, Warning, Error };

struct VerificationFinding {
    FindingSeverity severity = FindingSeverity::Error;
    std::string code;       // stable identifier for logs and the job database
    std::string message;    // what was found, in plain language
    std::string remedy;     // what MonoSect did about it, or what the user can do
};

struct VerificationPolicy {
    /// Hard limit checks.  These are on by default and turning them off is not
    /// offered in the UI: an oversized part is a defect, not a preference.
    bool enforceSizeLimit = true;
    bool enforceDurationLimit = true;
    /// Compare stream signatures against the source.
    bool checkStreamSignatures = true;
    /// Require the first video packet of a copied part to be a keyframe.  A part
    /// that does not start at a keyframe cannot be played on its own.
    bool requireKeyframeStart = true;
    /// Optional, slow: decode the part from start to end.
    bool decodeCheck = false;
    /// Optional: hash the part's streams and compare with the source range.
    bool contentHashCheck = false;
    /// Coverage check: the parts must cover the source without a gap.
    bool checkCoverage = true;
    /// Overlaps and rounding differences below this are reported as information
    /// rather than as a defect.  Zero means "no overlap at all is tolerated".
    Rational coverageTolerance{1, 100};   // 10 ms
};

/// What was measured on disk for one part.
struct SegmentMeasurement {
    std::string path;
    bool exists = false;
    bool readable = false;
    ByteCount fileSizeBytes = 0;
    bool durationKnown = false;
    Rational duration;
    std::vector<StreamSignature> signatures;
    bool firstVideoPacketIsKeyframe = false;
    bool firstVideoPacketChecked = false;
    std::optional<ByteCount> exactPayloadBytes;      // sum of packet sizes, when re-indexed
    std::optional<std::string> contentHash;
    std::vector<std::string> notes;

    /// Overhead actually taken by the container, when the payload was measured.
    [[nodiscard]] std::optional<ByteCount> measuredOverhead() const {
        if (!exactPayloadBytes.has_value()) {
            return std::nullopt;
        }
        return fileSizeBytes - *exactPayloadBytes;
    }
};

struct VerificationResult {
    bool passed = true;
    std::vector<VerificationFinding> findings;
    std::string summary;

    [[nodiscard]] std::size_t errorCount() const;
    [[nodiscard]] std::size_t warningCount() const;
    [[nodiscard]] std::string renderFindings() const;
    void add(FindingSeverity severity, std::string code, std::string message, std::string remedy = {});
};

/// Verifies one written part against the plan.
[[nodiscard]] VerificationResult evaluateSegment(const SplitPlan& plan, const SegmentPlan& segment,
                                                 const SegmentMeasurement& measurement,
                                                 const VerificationPolicy& policy);

/// Checks that the planned parts cover the source without gaps (a plan defect,
/// checked before writing anything).
[[nodiscard]] VerificationResult evaluatePlanCoverage(const SplitPlan& plan, const VerificationPolicy& policy);

/// Compares the source file with the parts that were written: all parts present,
/// and the total duration matches the source within the coverage tolerance.
[[nodiscard]] VerificationResult evaluateSequence(const SplitPlan& plan,
                                                  const std::vector<SegmentMeasurement>& measurements,
                                                  const VerificationPolicy& policy);

/// Record of what happened to one part, for the job history.
struct SegmentOutcome {
    int index = 0;
    bool wrote = false;
    std::string path;
    ByteCount fileSizeBytes = 0;
    Rational duration;                 // measured duration of the part
    Rational startSeconds;
    Rational endSeconds;
    SegmentMethod method = SegmentMethod::Copy;
    bool verified = false;
    bool committed = false;
    std::string verificationSummary;
    double elapsedSeconds = 0.0;
    std::vector<std::string> findings;
    [[nodiscard]] std::string toJson() const;
};

} // namespace monosect::core
