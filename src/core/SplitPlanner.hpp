// MonoSect — the split planner.
//
// The planner is a pure function of (source index, constraint, naming, size
// model).  It never runs FFmpeg, never touches the disk and never guesses about
// the result: everything it promises is computed from the packet index, and
// everything it cannot know exactly is marked as a prediction and verified after
// writing.
//
// How a boundary is chosen:
//   1. the payload budget is turned into a payload in bytes
//      (limit minus the container overhead allowance),
//   2. the cumulative payload curve of the source is searched for the largest
//      position that stays inside the budget (this is a binary search over the
//      candidate grid, so planning stays fast on large files),
//   3. the duration limit, the target duration and the minimum part size are
//      applied as exact rational comparisons,
//   4. in the copy modes the position is snapped back to the closest keyframe,
//      because a copied part can only start at a keyframe; the difference from
//      the requested position is stored in the plan and reported.
#pragma once

#include "core/Constraints.hpp"
#include "core/MediaModel.hpp"
#include "core/OutputNaming.hpp"
#include "core/PacketIndex.hpp"
#include "core/SizeModel.hpp"
#include "core/SplitPlan.hpp"

#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

struct PlannerSettings {
    /// A part smaller than this is not worth creating; the planner merges it into
    /// the previous part when that does not violate a hard limit, and otherwise
    /// explains why it stopped.
    Rational minimumPartDuration{1, 4};
    /// Same idea, expressed in bytes.
    ByteCount minimumPartBytes = 512 * kKiB;
    /// Sanity limit; a plan that needs more parts is refused.
    int maximumParts = 100000;
    /// Extra safety subtracted from a duration limit when writing, because the
    /// container duration of a copied part is the last frame's end, which can be
    /// up to one frame beyond the requested end.
    bool subtractFrameDurationFromDurationLimit = true;
};

struct PlannerStats {
    int parts = 0;
    int candidateEvaluations = 0;
    int keyframeSnapped = 0;
    int exactBoundaries = 0;
    int estimatedBoundaries = 0;
    Rational gridStep;                 // typical distance between grid points
    std::string gridDescription;       // where the grid came from
};

class SplitPlanner {
public:
    SplitPlanner(const MediaModel& model, const PacketIndex& index, const UserConstraint& constraint,
                 const NamingPolicy& naming, SizeModel sizeModel, PlannerSettings settings = {});

    /// Plans the whole source.
    [[nodiscard]] Result<SplitPlan> plan(const std::string& outputDirectory);

    /// Keeps the first `completedParts` parts of `previous` and plans the rest
    /// again with the current size model.
    [[nodiscard]] Result<SplitPlan> replanRemaining(const SplitPlan& previous, std::size_t completedParts);

    /// Plans one part beginning at `startSeconds`, used by the closeness pass to
    /// test whether a larger part is still safe.
    [[nodiscard]] Result<SegmentPlan> planOnePart(const SplitPlan& reference, Rational startSeconds, int index,
                                                   int totalHint, const std::string& outputDirectory);

    [[nodiscard]] const PlannerStats& stats() const noexcept { return stats_; }

    /// The payload budget that a part may use, given the hard limits.
    [[nodiscard]] ByteCount payloadBudget() const;

private:
    [[nodiscard]] Rational sourceEnd() const;
    [[nodiscard]] Rational gridTimebase() const;
    [[nodiscard]] const StreamPacketTrack* boundaryTrack() const;
    [[nodiscard]] std::vector<Rational> candidateBoundaries() const;
    [[nodiscard]] bool boundaryIsKeyframe(const Rational& time) const;
    [[nodiscard]] Rational nextKeyframeAtOrAfter(const Rational& time) const;
    [[nodiscard]] Rational keyframeAtOrBefore(const Rational& time) const;
    /// Smallest candidate boundary strictly after `time`, used to describe the
    /// smallest part the source can produce when a limit cannot be satisfied.
    [[nodiscard]] std::optional<Rational> nextCandidateAfter(const Rational& time) const;

    [[nodiscard]] ByteCount payloadInRange(const Rational& start, const Rational& end) const;
    [[nodiscard]] ByteCount predictedFileBytes(ByteCount payload) const;
    [[nodiscard]] Rational durationLimitFor(const Rational& start) const;

    struct Choice {
        Rational end;                 // where the part will actually end
        Rational requested;           // where the limit alone would have put it
        BoundaryReason reason = BoundaryReason::SourceEnd;
        BoundaryReason requestedReason = BoundaryReason::SizeLimit;
        std::string explanation;
        bool exact = false;
        bool found = false;
        bool requestedIsMeaningful = false;
    };

    /// Largest end position that satisfies every hard limit, or a failed choice.
    [[nodiscard]] Choice chooseEnd(const Rational& start, const Rational& hardEnd,
                                   bool allowExactTime) const;

    [[nodiscard]] SegmentPlan makeSegment(const SplitPlan& plan, const Rational& start, const Rational& end,
                                          int index, int total, const BoundaryPlan& boundary) const;

    /// Shared implementation of plan()/replanRemaining().
    [[nodiscard]] Result<SplitPlan> buildPlan(const std::string& outputDirectory, int firstIndex,
                                              Rational start, std::size_t keepParts,
                                              const SplitPlan* previous);

    [[nodiscard]] Rational frameDuration() const;

    const MediaModel& model_;
    const PacketIndex& index_;
    UserConstraint constraint_;
    NamingPolicy naming_;
    SizeModel sizeModel_;
    PlannerSettings settings_;
    mutable PlannerStats stats_;
};

} // namespace monosect::core
