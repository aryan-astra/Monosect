// MonoSect — the split plan.
//
// The plan is a data structure, not a script: it says exactly where each part
// starts and ends, why that boundary was chosen, how the part will be written,
// and how large and how long it is predicted to be.  The executor may only do
// what the plan says; the verifier checks the plan against the disk.
#pragma once

#include "core/Constraints.hpp"
#include "core/MediaModel.hpp"
#include "core/OutputNaming.hpp"
#include "core/SizeModel.hpp"

#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

/// Why a boundary sits where it sits.  This is shown to the user verbatim; there
/// is no "approximate" boundary that is not explained by one of these reasons.
enum class BoundaryReason {
    SourceStart,             // 0 (or the first packet of the file)
    SourceEnd,               // the end of the file
    ExactRequest,            // the user asked for this exact time and the mode allows it
    KeyframeSearch,          // the closest keyframe to the requested time
    SizeLimit,               // the largest position that keeps the part inside the size limit
    DurationLimit,           // the position where the duration limit is reached
    TargetDuration,          // the position where the target part duration is reached
    SplitPosition,           // the user's single split position
    MinimumPartSize,         // a limit-driven cut was raised to avoid an unusably small part
    SafetyMargin,            // a measured correction was applied after a part came out too large
    RemainingSource,         // the rest of the file is the final part
};

[[nodiscard]] const char* boundaryReasonName(BoundaryReason reason) noexcept;

struct BoundaryPlan {
    bool requested = false;          // false for the first boundary of the file
    Rational requestedSeconds;       // the position the user asked for (when requested)
    Rational actualSeconds;          // the exact presentation time of the first packet
    Rational delta;                  // actual - requested; zero when requested == actual
    bool exact = true;               // true only when delta is zero
    BoundaryReason reason = BoundaryReason::SourceStart;
    std::string explanation;         // one sentence, always filled in
};

enum class SegmentMethod {
    Copy,        // every packet copied
    SmartCut,    // copied body with a re-encoded head
    ReEncode,    // the whole part is re-encoded
};

[[nodiscard]] const char* segmentMethodName(SegmentMethod method) noexcept;

/// How many packets of one stream a part must contain.  For a copied stream this
/// is an exact requirement (the part contains a byte-identical subset of the
/// source packets) and verification enforces it; for a re-encoded stream it is
/// the number of source packets the part covers, which the encoder is expected to
/// reproduce and which verification reports if it does not.
struct StreamPacketCount {
    int streamIndex = -1;
    std::int64_t packetCount = 0;
    bool copied = true;
};

struct ReencodeRegion {
    Rational startSeconds;
    Rational endSeconds;
    std::string reason;      // why this region has to be re-encoded
};

struct SegmentPlan {
    int index = 0;                    // 1-based part number, as shown to the user
    int total = 0;                    // filled in when the plan is complete
    Rational startSeconds;
    Rational endSeconds;
    BoundaryPlan startBoundary;
    bool endsAtSourceEnd = false;
    SegmentMethod method = SegmentMethod::Copy;
    std::vector<ReencodeRegion> reencodeRegions;
    std::vector<int> copiedStreamIndexes;      // streams copied untouched
    std::vector<int> reencodedStreamIndexes;   // streams re-encoded
    // Parallel to reencodedStreamIndexes: what has to be encoded and how.
    std::vector<std::string> reencodedCodecNames;
    std::vector<StreamKind> reencodedKinds;
    std::vector<int> reencodedBitRates;

    // Predictions, all exact integers, computed before anything is written.
    ByteCount predictedPayloadBytes = 0;
    ByteCount predictedOverheadBytes = 0;
    ByteCount predictedFileBytes = 0;
    Rational predictedDuration;

    /// The exact packet counts the writer is told to produce, one entry per
    /// stream.  These are what makes a boundary exact instead of approximate:
    /// FFmpeg is asked for a packet count, not for a duration, so the part cannot
    /// grow or shrink by a frame.
    std::vector<StreamPacketCount> expectedPackets;

    /// The source streams this part is expected to carry, in source order.
    ///
    /// This is not always every stream of the source.  A stream that has no packet
    /// inside the part's range cannot appear in it - a subtitle track with three
    /// cues in a ten second file contributes nothing to the last part - so the
    /// verifier has to compare against what the part should contain rather than
    /// against the source's whole stream list.  Comparing against the source is
    /// what produced "the file has 4 stream(s) but 5 were expected" for a part that
    /// was written exactly as planned.
    ///
    /// Empty means "every source stream", which is the ordinary case and keeps
    /// plans produced before this field existed meaningful.
    std::vector<int> expectedStreamIndexes;

    /// Signatures of the source streams this part is expected to carry.
    std::vector<StreamSignature> expectedSignatures;

    std::string outputFileName;      // file name only, no directory
    std::vector<std::string> notes;  // per-part remarks shown in the plan table

    [[nodiscard]] bool isCopy() const noexcept { return method == SegmentMethod::Copy; }
    /// Expected packet count of one stream, or -1 when the plan does not say.
    [[nodiscard]] std::int64_t expectedPacketCount(int streamIndex) const;
};

struct SplitPlan {
    bool planned = false;
    std::string failureExplanation;

    std::string sourcePath;
    ByteCount sourceFileBytes = 0;
    std::string sourceFormatNames;
    Rational sourceDuration;
    bool sourceDurationKnown = false;
    bool sourceDurationExact = false;

    UserConstraint constraint;
    std::string outputDirectory;
    std::string outputExtension;
    Rational timebase;               // the grid the plan works on (video stream timebase)
    int boundaryStreamIndex = -1;    // the stream whose keyframes define copy boundaries

    std::vector<SegmentPlan> segments;

    ByteCount predictedTotalBytes = 0;
    Rational predictedTotalDuration;
    SizeModel sizeModel;

    /// Signatures of the source streams.  Verification compares the streams of
    /// every written part against this list.
    std::vector<StreamSignature> sourceSignatures;

    std::vector<std::string> warnings;
    std::vector<std::string> notes;

    [[nodiscard]] std::size_t partCount() const noexcept { return segments.size(); }
    [[nodiscard]] const SegmentPlan* segment(int index) const;
    /// Largest predicted size in the plan, or 0.
    [[nodiscard]] ByteCount largestPredictedBytes() const;
    /// Longest predicted duration in the plan.
    [[nodiscard]] Rational longestPredictedDuration() const;
    /// True when every predicted size is within the hard limit.
    [[nodiscard]] bool respectsHardSizeLimit() const;
    [[nodiscard]] bool respectsHardDurationLimit() const;

    /// Multi-line human summary used in the log, the report and the UI.
    [[nodiscard]] std::string describe() const;
    /// Machine form for the job database.
    [[nodiscard]] std::string toJson() const;
};

} // namespace monosect::core
