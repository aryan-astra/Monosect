#include "ui/UiModel.hpp"

#include "core/Bytes.hpp"
#include "core/ResourcePolicy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace monosect::core {
namespace {

std::string percentValue(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f%%", value);
    return buffer;
}

} // namespace

std::string percentText(const MeasuredValue& value) {
    if (!value.known) {
        return "n/a";
    }
    return percentValue(value.value);
}

std::string bytesPerSecondText(const MeasuredValue& value) {
    if (!value.known) {
        return "n/a";
    }
    return formatBytesShort(static_cast<ByteCount>(value.value)) + "/s";
}

std::string UiModel::meterText(const MeasuredValue& value, const char* unit, int decimals) {
    if (!value.known) {
        // The reason is shown as a tooltip, not invented as a number.
        return "n/a";
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f %s", decimals, value.value, unit);
    return buffer;
}

std::string UiModel::constraintHeadline(const UserConstraint& constraint) {
    if (!constraint.hasAnyHardLimit() && !constraint.splitAtSeconds.has_value()) {
        return "no limits set";
    }
    std::string text = describeConstraint(constraint);
    text.append("; ");
    text.append(preservationModeName(constraint.preservation));
    text.append(", ");
    text.append(precisionPolicyName(constraint.precision));
    return text;
}

std::string UiModel::limitExplanation(const UserConstraint& constraint) {
    if (!constraint.hasSizeLimit()) {
        return constraint.hasDurationLimit()
                   ? "No size limit: parts are bounded by time only."
                   : "No limit is set, so a single part would contain the whole file.";
    }
    std::string text = "The limit is stored as an exact byte count: ";
    text.append(formatBytesExact(constraint.size->maxBytes));
    if (constraint.size->minBytes.has_value()) {
        text.append(" (aiming for at least ");
        text.append(formatBytesExact(*constraint.size->minBytes));
        text.append(" so that parts are not needlessly small)");
    }
    text.append(". No part is ever published above it.");
    return text;
}

const HelpTopic* UiModel::helpForConstraint(const UserConstraint& constraint) {
    if (constraint.preservation == PreservationMode::SmartCut) {
        return HelpContent::find(HelpTopicId::SmartCut);
    }
    if (constraint.hasSizeLimit()) {
        return HelpContent::find(HelpTopicId::SizeLimit);
    }
    if (constraint.hasDurationLimit()) {
        return HelpContent::find(HelpTopicId::DurationLimit);
    }
    return HelpContent::find(HelpTopicId::FirstRun);
}

bool UiModel::boundaryIsExact(const SegmentPlan& segment) {
    // The plan records whether an exact boundary exists; the UI must not derive
    // its own answer from a rounded display value.
    if (!segment.startBoundary.exact) {
        return false;
    }
    // ... and it must actually be exact: the two times have to be equal.
    return compareRational(segment.startBoundary.actualSeconds, segment.startSeconds) == 0 &&
           (!segment.startBoundary.requested ||
            compareRational(segment.startBoundary.requestedSeconds, segment.startBoundary.actualSeconds) == 0);
}

std::vector<UiModel::PartLine> UiModel::partLines(const SplitPlan& plan) {
    std::vector<PartLine> lines;
    lines.reserve(plan.segments.size());
    for (const SegmentPlan& segment : plan.segments) {
        PartLine line;
        line.index = segment.index;
        line.range = formatDuration(segment.startSeconds) + " - " + formatDuration(segment.endSeconds);
        line.size = formatBytesShort(segment.predictedFileBytes) + " (" +
                    formatBytesExact(segment.predictedFileBytes) + " predicted)";
        line.method = segmentMethodName(segment.method);
        line.exactBoundary = boundaryIsExact(segment);
        if (!segment.startBoundary.requested) {
            line.boundary = "start of the file";
        } else {
            line.boundary = "requested " + formatDuration(segment.startBoundary.requestedSeconds) + ", actual " +
                            formatDuration(segment.startBoundary.actualSeconds);
            const Rational delta = subtractRational(segment.startBoundary.actualSeconds,
                                                    segment.startBoundary.requestedSeconds)
                                       .value_or(Rational{0, 1});
            if (!delta.isZero()) {
                const bool late = delta.isPositive();
                const Rational magnitude = late ? delta : Rational{-delta.numerator, delta.denominator};
                line.boundary += " (" + std::string(late ? "later" : "earlier") + " by " +
                                 formatDuration(magnitude) + ")";
            }
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string UiModel::planSummary(const SplitPlan& plan) {
    if (!plan.planned) {
        return "Not planned: " + plan.failureExplanation;
    }
    std::string text = std::to_string(plan.segments.size()) + " part(s)";
    if (!plan.segments.empty()) {
        text.append(", largest ");
        text.append(formatBytesShort(plan.largestPredictedBytes()));
        text.append(" predicted");
    }
    if (plan.constraint.hasSizeLimit()) {
        text.append(", limit ");
        text.append(formatBytesExact(plan.constraint.size->maxBytes));
        text.append(". ");
        text.append(plan.respectsHardSizeLimit() ? "Every part is predicted inside the limit."
                                                 : "A part is predicted over the limit - this plan will be refused.");
    }
    if (plan.constraint.hasDurationLimit()) {
        text.append(" Duration limit ");
        text.append(formatDuration(plan.constraint.duration->maxSeconds));
        text.append(" per part; ");
        text.append(plan.respectsHardDurationLimit() ? "each part is predicted inside it."
                                                     : "a part is predicted longer than it.");
    }
    if (!plan.sizeModel.calibrated()) {
        text.append(" Sizes are estimates from the source index until the first part is measured.");
    } else {
        text.append(" Sizes use the measured overhead of ");
        text.append(std::to_string(plan.sizeModel.observationCount()));
        text.append(" written part(s).");
    }
    return text;
}

std::vector<std::string> UiModel::planCaveats(const SplitPlan& plan) {
    std::vector<std::string> caveats;
    if (!plan.planned) {
        caveats.push_back(plan.failureExplanation);
        return caveats;
    }
    for (const SegmentPlan& segment : plan.segments) {
        if (segment.method == SegmentMethod::SmartCut) {
            std::string text = "part " + std::to_string(segment.index) + ": the region ";
            bool first = true;
            for (const ReencodeRegion& region : segment.reencodeRegions) {
                if (!first) {
                    text.append(", ");
                }
                text.append(formatDuration(region.startSeconds)).append(" - ").append(formatDuration(region.endSeconds));
                first = false;
            }
            text.append(" is re-encoded (copied elsewhere).");
            caveats.push_back(text);
        } else if (segment.method == SegmentMethod::ReEncode) {
            caveats.push_back("part " + std::to_string(segment.index) + ": the whole part is re-encoded.");
        }
        if (segment.startBoundary.requested && !boundaryIsExact(segment)) {
            caveats.push_back("part " + std::to_string(segment.index) + ": the cut is not exactly where the limit "
                              "asked for it - " + segment.startBoundary.explanation);
        }
    }
    for (const std::string& warning : plan.warnings) {
        caveats.push_back(warning);
    }
    return caveats;
}

JobStatusLine UiModel::verificationLine(const VerificationResult& result) {
    JobStatusLine line;
    if (result.passed) {
        line.headline = "Verified";
        line.detail = result.summary.empty() ? "every check passed" : result.summary;
        line.tone = StatusTone::Good;
        return line;
    }
    line.headline = "Not verified";
    line.tone = StatusTone::Bad;
    std::string first;
    for (const VerificationFinding& finding : result.findings) {
        if (finding.severity == FindingSeverity::Error) {
            first = finding.message;
            break;
        }
    }
    line.detail = first.empty() ? result.summary : first;
    return line;
}

JobStatusLine UiModel::jobLine(const std::string& message, bool succeeded, bool canceled, bool verified) {
    JobStatusLine line;
    line.detail = message;
    if (canceled) {
        line.headline = "Cancelled";
        line.tone = StatusTone::Warning;
        return line;
    }
    if (!succeeded) {
        line.headline = "Failed";
        line.tone = StatusTone::Bad;
        return line;
    }
    if (!verified) {
        // A job that claims success without a verification result would be
        // claiming something that was not checked.
        line.headline = "Finished (not verified)";
        line.tone = StatusTone::Warning;
        return line;
    }
    line.headline = "Finished and verified";
    line.tone = StatusTone::Good;
    return line;
}

ProgressView UiModel::progress(int partsDone, int partsTotal, double bytesWritten,
                               std::optional<double> remainingSeconds) {
    ProgressView view;
    view.partsDone = partsDone;
    view.partsTotal = partsTotal;
    view.bytesWritten = bytesWritten;
    view.fraction = partsTotal > 0 ? std::min(1.0, static_cast<double>(partsDone) / partsTotal) : 0.0;
    if (remainingSeconds.has_value() && *remainingSeconds >= 0.0) {
        view.secondsRemaining = remainingSeconds;
    } else if (partsTotal == 0) {
        view.etaExplanation = "The number of parts is not known yet.";
    } else if (partsDone == 0) {
        view.etaExplanation = "No part has finished yet, so a time estimate would be a guess.";
    } else {
        view.etaExplanation = "The remaining parts have no trustworthy duration yet.";
    }
    return view;
}

std::string ProgressView::describe() const {
    if (partsTotal == 0) {
        return "no part count yet";
    }
    std::string text = std::to_string(partsDone) + " of " + std::to_string(partsTotal) + " part(s) done";
    if (bytesWritten > 0.0) {
        text.append(", ");
        text.append(formatBytesShort(static_cast<ByteCount>(bytesWritten)));
        text.append(" written");
    }
    if (secondsRemaining.has_value()) {
        text.append(", about ");
        text.append(formatDuration(Rational{static_cast<std::int64_t>(*secondsRemaining * 1000 + 0.5), 1000}));
        text.append(" remaining");
    } else if (!etaExplanation.empty()) {
        text.append(" (no estimate: ");
        text.append(etaExplanation);
        text.append(")");
    }
    return text;
}

std::string UiModel::resourcePolicySummary(const ResourcePolicy& policy) {
    std::string text = resourceModeName(policy.mode);
    text.append(": ");
    text.append(std::to_string(policy.encoderThreads));
    text.append(" encoder thread(s)");
    if (policy.filterThreads > 0) {
        text.append(", ");
        text.append(std::to_string(policy.filterThreads));
        text.append(" filter thread(s)");
    }
    if (policy.useGpuEncoder && !policy.gpuEncoder.empty()) {
        text.append(", GPU encoder ");
        text.append(policy.gpuEncoder);
    }
    text.append(". ");
    text.append(policy.rationale);
    return text;
}

} // namespace monosect::core
