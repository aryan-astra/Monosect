#include "core/Verification.hpp"

#include "core/Json.hpp"

#include <algorithm>
#include <sstream>

namespace monosect::core {
namespace {

std::string sizeComparisonText(ByteCount actual, ByteCount limit) {
    return formatBytesExact(actual) + " against a limit of " + formatBytesExact(limit);
}

} // namespace

std::size_t VerificationResult::errorCount() const {
    return static_cast<std::size_t>(std::count_if(findings.begin(), findings.end(),
                                                  [](const VerificationFinding& finding) {
                                                      return finding.severity == FindingSeverity::Error;
                                                  }));
}

std::size_t VerificationResult::warningCount() const {
    return static_cast<std::size_t>(std::count_if(findings.begin(), findings.end(),
                                                  [](const VerificationFinding& finding) {
                                                      return finding.severity == FindingSeverity::Warning;
                                                  }));
}

void VerificationResult::add(FindingSeverity severity, std::string code, std::string message, std::string remedy) {
    if (severity == FindingSeverity::Error) {
        passed = false;
    }
    findings.push_back(VerificationFinding{severity, std::move(code), std::move(message), std::move(remedy)});
}

std::string VerificationResult::renderFindings() const {
    std::ostringstream text;
    for (const auto& finding : findings) {
        switch (finding.severity) {
            case FindingSeverity::Error: text << "error: "; break;
            case FindingSeverity::Warning: text << "warning: "; break;
            case FindingSeverity::Info: text << "info: "; break;
        }
        text << finding.message;
        if (!finding.remedy.empty()) {
            text << " (" << finding.remedy << ")";
        }
        text << "\n";
    }
    return text.str();
}

VerificationResult evaluateSegment(const SplitPlan& plan, const SegmentPlan& segment,
                                   const SegmentMeasurement& measurement, const VerificationPolicy& policy) {
    VerificationResult result;
    result.summary = "part " + std::to_string(segment.index) + ": ";

    if (!measurement.exists) {
        result.add(FindingSeverity::Error, "missing", "The part was not written to disk.", measurement.path);
        result.summary += "not written";
        return result;
    }
    if (!measurement.readable) {
        result.add(FindingSeverity::Error, "unreadable",
                   "The written part could not be read back as a media file.",
                   measurement.path + " - the part is not published");
        result.summary += "unreadable";
        return result;
    }

    // 1. Hard size limit: exact integer comparison, no tolerance.
    if (policy.enforceSizeLimit && plan.constraint.hasSizeLimit()) {
        const ByteCount limit = plan.constraint.size->maxBytes;
        if (measurement.fileSizeBytes > limit) {
            result.add(FindingSeverity::Error, "size_limit_exceeded",
                       "The part is larger than the hard size limit: " +
                           sizeComparisonText(measurement.fileSizeBytes, limit),
                       "the part is discarded and written again with a smaller boundary");
        } else {
            const ByteCount slack = limit - measurement.fileSizeBytes;
            result.add(FindingSeverity::Info, "size_ok",
                       "Size " + sizeComparisonText(measurement.fileSizeBytes, limit) + " (" +
                           formatBytesShort(slack) + " unused)",
                       {});
        }
    }

    // 2. Hard duration limit: exact rational comparison.
    if (policy.enforceDurationLimit && plan.constraint.hasDurationLimit()) {
        if (!measurement.durationKnown) {
            result.add(FindingSeverity::Error, "duration_unreadable",
                       "The part does not report a duration, so the duration limit could not be verified.",
                       "the part is discarded");
        } else if (compareRational(measurement.duration, plan.constraint.duration->maxSeconds) > 0) {
            result.add(FindingSeverity::Error, "duration_limit_exceeded",
                       "The part is longer than the hard duration limit: " + formatDuration(measurement.duration) +
                           " against a limit of " + formatDuration(plan.constraint.duration->maxSeconds),
                       "the part is discarded and written again with a smaller boundary");
        } else {
            result.add(FindingSeverity::Info, "duration_ok",
                       "Duration " + formatDuration(measurement.duration) + " is inside the limit of " +
                           formatDuration(plan.constraint.duration->maxSeconds),
                       {});
        }
    }

    // 3. Stream signatures.
    if (policy.checkStreamSignatures) {
        // The expectation is what this part should carry, which is not always the
        // source's whole stream list: a stream with no packet inside the part's
        // range cannot be in the part.  Falling back to the source list keeps plans
        // that do not name their streams comparable.
        const std::vector<StreamSignature>& expected =
            segment.expectedSignatures.empty() ? plan.sourceSignatures : segment.expectedSignatures;
        const std::vector<std::string> differences = compareSignatureSets(expected, measurement.signatures);
        for (const std::string& difference : differences) {
            result.add(FindingSeverity::Error, "stream_mismatch",
                       "The written part does not carry the same streams as the source: " + difference,
                       "the part is discarded; this is a defect that should be reported");
        }
        if (differences.empty()) {
            result.add(FindingSeverity::Info, "streams_ok",
                       "all " + std::to_string(expected.size()) +
                           " stream(s) are present with the expected codec parameters",
                       {});
        }
    }

    // 4. A copied part has to start at a keyframe to be playable on its own.
    if (policy.requireKeyframeStart && measurement.firstVideoPacketChecked && segment.isCopy() &&
        !measurement.firstVideoPacketIsKeyframe) {
        result.add(FindingSeverity::Error, "not_keyframe_start",
                   "The part does not start at a keyframe, so it cannot be played on its own.",
                   "the boundary is moved to the previous keyframe and the part is written again");
    }

    // 5. Planned against measured boundaries.
    if (measurement.durationKnown && segment.predictedDuration.isPositive()) {
        const Rational difference = subtractRational(measurement.duration, segment.predictedDuration)
                                        .value_or(Rational{0, 1});
        const Rational tolerance = policy.coverageTolerance;
        const bool beyondTolerance = compareRational(difference, tolerance) > 0 ||
                                     compareRational(difference, Rational{-tolerance.numerator, tolerance.denominator}) < 0;
        if (beyondTolerance) {
            result.add(FindingSeverity::Warning, "duration_prediction_off",
                       "The part is " + formatDuration(measurement.duration) + " long, but the plan predicted " +
                           formatDuration(segment.predictedDuration) + ".",
                       "this is recorded; the plan for the following parts uses the measured values");
        }
    }

    for (const std::string& note : measurement.notes) {
        result.add(FindingSeverity::Info, "measurement_note", note, {});
    }

    result.summary += result.passed ? "verified" : "FAILED";
    if (!result.passed) {
        result.summary += " (" + std::to_string(result.errorCount()) + " error(s))";
    }
    return result;
}

VerificationResult evaluatePlanCoverage(const SplitPlan& plan, const VerificationPolicy& policy) {
    VerificationResult result;
    if (!policy.checkCoverage) {
        result.summary = "coverage check disabled";
        return result;
    }
    const Rational tolerance = policy.coverageTolerance;
    for (std::size_t i = 1; i < plan.segments.size(); ++i) {
        const Rational previousEnd = plan.segments[i - 1].endSeconds;
        const Rational nextStart = plan.segments[i].startSeconds;
        const Rational difference = subtractRational(nextStart, previousEnd).value_or(Rational{0, 1});
        if (compareRational(difference, tolerance) > 0) {
            result.add(FindingSeverity::Error, "plan_gap",
                       "The plan leaves a gap of " + formatDuration(difference) + " between part " +
                           std::to_string(plan.segments[i - 1].index) + " and part " +
                           std::to_string(plan.segments[i].index) + ".",
                       "this is a planner defect; the job is not started");
        } else if (compareRational(difference, Rational{-tolerance.numerator, tolerance.denominator}) < 0) {
            result.add(FindingSeverity::Warning, "plan_overlap",
                       "The plan overlaps part " + std::to_string(plan.segments[i - 1].index) + " and part " +
                           std::to_string(plan.segments[i].index) + " by " + formatDuration(difference) + ".",
                       "the overlapping frames will appear in both parts");
        }
    }
    if (!plan.segments.empty()) {
        // Internal continuity is not enough: the plan says where the first part
        // starts and whether the last one reaches the end, so both are checked.
        // A plan that quietly drops the head or the tail of the file would
        // otherwise look continuous.
        const SegmentPlan& first = plan.segments.front();
        if (first.startBoundary.reason != BoundaryReason::SourceStart) {
            result.add(FindingSeverity::Error, "plan_start",
                       "The plan starts at " + formatDuration(first.startSeconds) +
                           " instead of the beginning of the source (the boundary is recorded as \"" +
                           std::string(boundaryReasonName(first.startBoundary.reason)) + "\").",
                       "this is a planner defect; the job is not started");
        }
        const SegmentPlan& last = plan.segments.back();
        if (!last.endsAtSourceEnd) {
            result.add(FindingSeverity::Error, "plan_end",
                       "The plan stops at " + formatDuration(last.endSeconds) +
                           " and does not mark the last part as reaching the end of the source.",
                       "this is a planner defect; the job is not started");
        }
    }
    if (!plan.segments.empty()) {
        result.add(FindingSeverity::Info, "plan_covers_source",
                   "the plan covers " + formatDuration(plan.segments.front().startSeconds) + " to " +
                       formatDuration(plan.segments.back().endSeconds) + " in " +
                       std::to_string(plan.segments.size()) + " part(s)",
                   {});
    }
    result.summary = result.passed ? "the plan is continuous" : "the plan has a gap";
    return result;
}

VerificationResult evaluateSequence(const SplitPlan& plan, const std::vector<SegmentMeasurement>& measurements,
                                    const VerificationPolicy& policy) {
    VerificationResult result;
    if (measurements.size() != plan.segments.size()) {
        result.add(FindingSeverity::Error, "incomplete",
                   "The job produced " + std::to_string(measurements.size()) + " part(s) but the plan has " +
                       std::to_string(plan.segments.size()) + ".",
                   "the job is not complete; missing parts are re-planned");
    }

    Rational total{0, 1};
    for (const auto& measurement : measurements) {
        if (measurement.durationKnown) {
            if (auto sum = addRational(total, measurement.duration); sum.has_value()) {
                total = sum.value();
            }
        }
    }
    if (plan.sourceDurationKnown && plan.sourceDurationExact) {
        const Rational difference = subtractRational(total, plan.sourceDuration).value_or(Rational{0, 1});
        const bool tooShort = compareRational(difference, Rational{-policy.coverageTolerance.numerator,
                                                                  policy.coverageTolerance.denominator}) < 0;
        const bool tooLong = compareRational(difference, policy.coverageTolerance) > 0;
        if (tooShort) {
            result.add(FindingSeverity::Error, "total_shorter_than_source",
                       "The parts add up to " + formatDuration(total) + " but the source is " +
                           formatDuration(plan.sourceDuration) + "; about " +
                           formatDuration(Rational{-difference.numerator, difference.denominator}) +
                           " of the source is missing.",
                       "the job is not reported as a success; re-run it or split the missing range again");
        } else if (tooLong) {
            result.add(FindingSeverity::Warning, "total_longer_than_source",
                       "The parts add up to " + formatDuration(total) + " but the source is " +
                           formatDuration(plan.sourceDuration) + "; " + formatDuration(difference) +
                           " is repeated at the boundaries.",
                       "this comes from keyframe alignment and is reported, not hidden");
        } else {
            result.add(FindingSeverity::Info, "total_matches_source",
                       "the parts add up to " + formatDuration(total) + ", which matches the source duration",
                       {});
        }
    } else {
        result.add(FindingSeverity::Info, "source_duration_unknown",
                   "the source duration is not known exactly, so only the parts were verified individually",
                   {});
    }
    result.summary = result.passed ? "the sequence is complete" : "the sequence is incomplete";
    return result;
}

std::string SegmentOutcome::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root.set("index", JsonValue::makeInt(index));
    root.set("wrote", JsonValue::makeBool(wrote));
    root.set("path", JsonValue::makeString(path));
    root.set("bytes", JsonValue::makeInt(fileSizeBytes));
    JsonValue duration = JsonValue::makeObject();
    duration.set("num", JsonValue::makeInt(this->duration.numerator));
    duration.set("den", JsonValue::makeInt(this->duration.denominator));
    root.set("duration", std::move(duration));
    JsonValue start = JsonValue::makeObject();
    start.set("num", JsonValue::makeInt(startSeconds.numerator));
    start.set("den", JsonValue::makeInt(startSeconds.denominator));
    root.set("start", std::move(start));
    JsonValue end = JsonValue::makeObject();
    end.set("num", JsonValue::makeInt(endSeconds.numerator));
    end.set("den", JsonValue::makeInt(endSeconds.denominator));
    root.set("end", std::move(end));
    root.set("method", JsonValue::makeString(segmentMethodName(method)));
    root.set("verified", JsonValue::makeBool(verified));
    root.set("committed", JsonValue::makeBool(committed));
    root.set("verification", JsonValue::makeString(verificationSummary));
    root.set("elapsed_seconds", JsonValue::makeDouble(elapsedSeconds));
    JsonValue findings = JsonValue::makeArray();
    for (const std::string& finding : this->findings) {
        findings.push(JsonValue::makeString(finding));
    }
    root.set("findings", std::move(findings));
    return root.dump();
}

} // namespace monosect::core
