// MonoSect — the presentation model (TODO 272 - TODO 274, TODO 291 - TODO 299).
//
// Everything the window shows that is not a raw value comes from here: the
// status of a job, the wording of a constraint, the requested-vs-actual boundary
// line, the verification summary, the resource meters and the progress/ETA
// arithmetic.  It is deliberately free of ImGui, GLFW and Windows headers:
//
//   * the same text can be unit-tested on the development machine, which is the
//     only way to check that the UI never claims more than the engine did;
//   * the wording lives in one place, so "verified", "exact" and "copied" mean
//     the same thing in the window, the log and the job history.
//
// The rule the tests enforce: this file may only say a thing is verified when a
// verification result says so, and may only call a boundary exact when the plan
// says the actual time equals the requested time.
#pragma once

#include "core/Constraints.hpp"
#include "core/SplitPlan.hpp"
#include "core/ResourcePolicy.hpp"
#include "core/Verification.hpp"
#include "ui/HelpContent.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

/// A measured resource value.  An unmeasurable metric is `known == false` and is
/// displayed as "n/a" - never as zero (TODO 206, 207).
struct MeasuredValue {
    double value = 0.0;
    bool known = false;
    std::string unavailableReason;

    [[nodiscard]] static MeasuredValue unknown(std::string reason) {
        MeasuredValue value;
        value.known = false;
        value.unavailableReason = std::move(reason);
        return value;
    }
    [[nodiscard]] static MeasuredValue of(double measured) {
        MeasuredValue value;
        value.value = measured;
        value.known = true;
        return value;
    }
};

struct ResourceSample {
    MeasuredValue cpuPercent;
    MeasuredValue memoryPercent;
    MeasuredValue gpuPercent;
    MeasuredValue diskReadBytesPerSecond;
    MeasuredValue diskWriteBytesPerSecond;
    MeasuredValue mediaThroughputBytesPerSecond;   // bytes of media written per second
    std::string sourceDescription;                 // where the numbers come from
};

/// How the UI words the state of a job.  `detail` is always shown; `tone` only
/// changes colour.
enum class StatusTone { Neutral, Working, Good, Warning, Bad };

struct JobStatusLine {
    std::string headline;
    std::string detail;
    StatusTone tone = StatusTone::Neutral;
    [[nodiscard]] bool empty() const noexcept { return headline.empty() && detail.empty(); }
};

/// Progress of the running job.  ETA is only produced when it is meaningful:
/// with no completed part, or with a part whose duration is unknown, the UI
/// shows the work done and no estimate.
struct ProgressView {
    int partsDone = 0;
    int partsTotal = 0;
    double fraction = 0.0;             // 0..1, by completed parts
    double bytesWritten = 0.0;
    std::optional<double> secondsRemaining;   // only when it can be computed
    std::string etaExplanation;               // why there is no ETA, when there is none

    [[nodiscard]] std::string describe() const;
};

class UiModel {
public:
    // ---- wording of the constraints -------------------------------------------------
    /// "at most 2.00 GiB (2,147,483,648 bytes) per part, keyframe-aligned copy".
    [[nodiscard]] static std::string constraintHeadline(const UserConstraint& constraint);
    /// The exact-size line under the limit field: what "2 GiB" really means.
    [[nodiscard]] static std::string limitExplanation(const UserConstraint& constraint);
    /// The help topic that belongs to a control.
    [[nodiscard]] static const HelpTopic* helpForConstraint(const UserConstraint& constraint);

    // ---- the plan preview -----------------------------------------------------------
    /// One line per part, in the form the plan table shows.
    struct PartLine {
        int index = 0;
        std::string range;             // "0:00.000 - 0:02.000"
        std::string size;              // "201.4 KiB (206,233 bytes)"
        std::string method;
        std::string boundary;          // requested vs actual, or "start of file"
        bool exactBoundary = false;
    };
    [[nodiscard]] static std::vector<PartLine> partLines(const SplitPlan& plan);
    /// The summary above the table: how many parts, what the largest one is, and
    /// whether every prediction is inside the limit.
    [[nodiscard]] static std::string planSummary(const SplitPlan& plan);
    /// What the plan will not do, in the user's words (mode, re-encode regions).
    [[nodiscard]] static std::vector<std::string> planCaveats(const SplitPlan& plan);
    /// True when the words "exact boundary" may be used for this part.
    [[nodiscard]] static bool boundaryIsExact(const SegmentPlan& segment);

    // ---- verification ----------------------------------------------------------------
    /// The verification result as the results panel shows it.  It never says
    /// "verified" unless the result passed.
    [[nodiscard]] static JobStatusLine verificationLine(const VerificationResult& result);
    /// The sequence-level status of a finished job.
    [[nodiscard]] static JobStatusLine jobLine(const std::string& message, bool succeeded, bool canceled,
                                               bool verified);

    // ---- resources, progress ---------------------------------------------------------
    [[nodiscard]] static std::string meterText(const MeasuredValue& value, const char* unit, int decimals = 0);
    /// Progress from measured part results.  `remainingSeconds` is used only when
    /// the caller knows a trustworthy duration for the remaining work.
    [[nodiscard]] static ProgressView progress(int partsDone, int partsTotal, double bytesWritten,
                                               std::optional<double> remainingSeconds);
    [[nodiscard]] static std::string resourcePolicySummary(const ResourcePolicy& policy);
};

/// Turns a machine measurement into the string the meter shows.
[[nodiscard]] std::string percentText(const MeasuredValue& value);
[[nodiscard]] std::string bytesPerSecondText(const MeasuredValue& value);

} // namespace monosect::core
