// MonoSect — user constraints and their validation.
//
// The single most important rule of the product lives here: a size limit is an
// exact number of bytes and a duration limit is an exact rational number of
// seconds.  Nothing in this file ever relaxes them.  When a constraint cannot be
// met, validation or planning fails and says why.
#pragma once

#include "core/Bytes.hpp"
#include "core/MediaModel.hpp"
#include "core/Rational.hpp"

#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

/// How the user is willing to have the source touched.
enum class PreservationMode {
    /// Copy every packet as it is.  Boundaries must be existing keyframes.
    StreamCopy,
    /// Copy every packet, but place boundaries as close as possible to the
    /// requested position by choosing among keyframes.  Lossless, not exact.
    KeyframeAligned,
    /// Copy the body of each part, re-encode only the small region around each
    /// boundary so the boundary can be exact.  Not bit-identical.
    SmartCut,
    /// Re-encode everything.  Used when the container or codec has to change, or
    /// when the user accepts it.  Not bit-identical.
    FullReEncode,
};

[[nodiscard]] const char* preservationModeName(PreservationMode mode) noexcept;
/// One sentence stating exactly what the mode preserves and what it does not.
[[nodiscard]] const char* preservationModeGuarantee(PreservationMode mode) noexcept;
/// "Copying is bit-identical", "Smart cut re-encodes ~1-3 s around each boundary".
[[nodiscard]] std::string preservationModeDetail(PreservationMode mode);

/// How hard the planner works to approach the limit.  This never changes the
/// hard limits; it only decides how much extra work is spent getting close.
enum class PrecisionPolicy {
    Fast,       // one pass, no calibration render
    Balanced,   // calibration from the first written part, one refinement per part
    Maximum,    // calibration render plus a binary search for the largest safe part
};

[[nodiscard]] const char* precisionPolicyName(PrecisionPolicy policy) noexcept;
[[nodiscard]] std::string precisionPolicyDetail(PrecisionPolicy policy);

struct SizeLimit {
    ByteCount maxBytes = 0;                    // hard maximum, exact
    std::optional<ByteCount> minBytes;         // optional lower bound for closeness
};

struct DurationLimit {
    Rational maxSeconds{0, 1};                 // hard maximum, exact rational
};

struct UserConstraint {
    std::optional<SizeLimit> size;             // unset = no size limit
    std::optional<DurationLimit> duration;     // unset = no duration limit
    PreservationMode preservation = PreservationMode::KeyframeAligned;
    PrecisionPolicy precision = PrecisionPolicy::Balanced;

    /// Streams that must be present in every part.  Empty means "all streams of
    /// the source", which is the default and the documented product promise.
    std::vector<int> requiredStreamIndexes;

    /// When set, the source is cut at this exact position instead of being split
    /// into parts of roughly equal size/duration.
    std::optional<Rational> splitAtSeconds;
    /// When set, the plan aims for parts of at most this duration (a soft target;
    /// the hard limits are in `duration`/`size`).
    std::optional<Rational> targetPartSeconds;
    /// Optional limit on the number of parts.  When the plan needs more parts
    /// than this to respect the hard limits, planning fails and explains it.
    std::optional<int> maxParts;

    [[nodiscard]] bool hasSizeLimit() const noexcept { return size.has_value() && size->maxBytes > 0; }
    [[nodiscard]] bool hasDurationLimit() const noexcept {
        return duration.has_value() && duration->maxSeconds.isPositive();
    }
    [[nodiscard]] bool hasAnyHardLimit() const noexcept { return hasSizeLimit() || hasDurationLimit(); }
};

struct ConstraintIssue {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    std::string message;     // what is wrong
    std::string remedy;      // what the user can do
};

struct ConstraintReport {
    bool valid = true;
    std::vector<ConstraintIssue> issues;
};

/// Checks a constraint against a concrete source file.  This runs before any
/// work is started, so an impossible request fails immediately with a reason
/// instead of failing after an hour of copying.
[[nodiscard]] ConstraintReport validateConstraint(const UserConstraint& constraint, const MediaModel& model);

/// True when the target container can hold this codec without re-encoding.
///
/// `sourcePath` (optional) is the file the format names came from.  It matters
/// because ffprobe calls the Matroska demuxer "matroska,webm" for every Matroska
/// file: without the path an .mkv would be judged as WebM and H.264 or HEVC
/// would be reported as impossible to copy, which is wrong.
[[nodiscard]] bool containerAcceptsCodec(std::string_view formatNames, std::string_view codecName,
                                         StreamKind kind, std::string_view sourcePath = {});

/// Human explanation of why a codec cannot be copied into a container.
[[nodiscard]] std::string codecCompatibilityNote(std::string_view formatNames, std::string_view codecName,
                                                 StreamKind kind, std::string_view sourcePath = {});

/// The streams that must be preserved, resolved against the model.
[[nodiscard]] std::vector<const StreamModel*> requiredStreams(const UserConstraint& constraint,
                                                              const MediaModel& model);

/// A short summary of the constraint for logs and the job database.
[[nodiscard]] std::string describeConstraint(const UserConstraint& constraint);

} // namespace monosect::core
