// MonoSect — the verification rules (TODO 243-257).
#include "TestSupport.hpp"
#include "Fixtures.hpp"

#include "core/SizeModel.hpp"
#include "core/SplitPlanner.hpp"
#include "core/Verification.hpp"

using namespace monosect::core;
using namespace monosect::test;

namespace {

SplitPlan smallPlan(const SyntheticSource& source) {
    SizeModel sizeModel;
    sizeModel.seedFromSource(source.model.formatNames, source.model.fileSize, source.index.totalPayloadBytes());
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 200000, .minBytes = std::nullopt};
    constraint.preservation = PreservationMode::KeyframeAligned;
    NamingPolicy naming;
    SplitPlanner planner(source.model, source.index, constraint, naming, sizeModel);
    auto plan = planner.plan("out");
    CHECK(plan.has_value());
    return plan.has_value() ? plan.value() : SplitPlan{};
}

SegmentMeasurement goodMeasurement(const SplitPlan& plan, const SegmentPlan& segment) {
    SegmentMeasurement measurement;
    measurement.path = "part.mp4";
    measurement.exists = true;
    measurement.readable = true;
    measurement.fileSizeBytes = segment.predictedFileBytes;
    measurement.duration = segment.predictedDuration;
    measurement.durationKnown = true;
    measurement.signatures = plan.sourceSignatures;
    measurement.firstVideoPacketChecked = true;
    measurement.firstVideoPacketIsKeyframe = true;
    return measurement;
}

} // namespace

MONOSECT_TEST(a_part_inside_the_limit_passes) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = smallPlan(source);
    VerificationPolicy policy;   // defaults: enforce size, duration, signatures, keyframe start
    const VerificationResult result = evaluateSegment(plan, plan.segments[0], goodMeasurement(plan, plan.segments[0]), policy);
    CHECK(result.passed);
    CHECK(result.renderFindings().size() > 0);
}

MONOSECT_TEST(one_byte_over_the_limit_fails_verification) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = smallPlan(source);
    SegmentMeasurement measurement = goodMeasurement(plan, plan.segments[0]);
    measurement.fileSizeBytes = plan.constraint.size->maxBytes + 1;   // exactly one byte over
    VerificationPolicy policy;
    const VerificationResult result = evaluateSegment(plan, plan.segments[0], measurement, policy);
    CHECK(!result.passed);
    bool mentioned = false;
    for (const VerificationFinding& finding : result.findings) {
        if (finding.message.find("limit") != std::string::npos) {
            mentioned = true;
        }
    }
    CHECK(mentioned);
}

MONOSECT_TEST(a_wrong_stream_fails_verification) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = smallPlan(source);
    SegmentMeasurement measurement = goodMeasurement(plan, plan.segments[0]);
    measurement.signatures[0].codecName = "hevc";
    VerificationPolicy policy;
    const VerificationResult result = evaluateSegment(plan, plan.segments[0], measurement, policy);
    CHECK(!result.passed);
}

MONOSECT_TEST(a_part_that_is_not_readable_fails) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = smallPlan(source);
    SegmentMeasurement measurement = goodMeasurement(plan, plan.segments[0]);
    measurement.readable = false;
    VerificationPolicy policy;
    CHECK(!evaluateSegment(plan, plan.segments[0], measurement, policy).passed);
    measurement.readable = true;
    measurement.exists = false;
    CHECK(!evaluateSegment(plan, plan.segments[0], measurement, policy).passed);
}

MONOSECT_TEST(a_part_that_does_not_start_on_a_keyframe_fails) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = smallPlan(source);
    SegmentMeasurement measurement = goodMeasurement(plan, plan.segments[1]);
    measurement.firstVideoPacketIsKeyframe = false;
    VerificationPolicy policy;
    CHECK(!evaluateSegment(plan, plan.segments[1], measurement, policy).passed);
}

MONOSECT_TEST(an_overlong_part_fails_the_duration_limit) {
    const SyntheticSource source = makeSyntheticSource();
    SizeModel sizeModel;
    sizeModel.seedFromSource(source.model.formatNames, source.model.fileSize, source.index.totalPayloadBytes());
    UserConstraint constraint;
    constraint.duration = DurationLimit{Rational{3, 1}};
    NamingPolicy naming;
    SplitPlanner planner(source.model, source.index, constraint, naming, sizeModel);
    auto plan = planner.plan("out");
    CHECK(plan.has_value());
    SegmentMeasurement measurement = goodMeasurement(plan.value(), plan.value().segments[0]);
    measurement.duration = Rational{4, 1};   // longer than the 3 s limit
    VerificationPolicy policy;
    const VerificationResult result = evaluateSegment(plan.value(), plan.value().segments[0], measurement, policy);
    CHECK(!result.passed);
}

MONOSECT_TEST(the_sequence_check_reports_a_total_duration_mismatch) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = smallPlan(source);
    std::vector<SegmentMeasurement> measurements;
    for (const SegmentPlan& segment : plan.segments) {
        measurements.push_back(goodMeasurement(plan, segment));
    }
    VerificationPolicy policy;
    CHECK(evaluateSequence(plan, measurements, policy).passed);

    // One part is far too short: the sum no longer matches the source, and the
    // difference is reported rather than hidden.
    measurements.back().duration = Rational{1, 10};
    const VerificationResult result = evaluateSequence(plan, measurements, policy);
    CHECK(!result.passed);
    CHECK(result.renderFindings().find("0:") != std::string::npos);
}

MONOSECT_TEST(the_plan_itself_must_cover_the_source) {
    const SyntheticSource source = makeSyntheticSource();
    SplitPlan plan = smallPlan(source);
    VerificationPolicy policy;
    CHECK(evaluatePlanCoverage(plan, policy).passed);

    // Move one part later: the plan now has a hole, which is a planning defect.
    plan.segments[1].startSeconds = addRational(plan.segments[1].startSeconds, (Rational{1, 1})).value();
    const VerificationResult gapped = evaluatePlanCoverage(plan, policy);
    CHECK(!gapped.passed);
    CHECK(gapped.renderFindings().find("gap") != std::string::npos);

    // A plan whose first part no longer starts at the source is also refused: the
    // dropped head would otherwise be invisible.
    SplitPlan headless = smallPlan(source);
    headless.segments.front().startBoundary.reason = BoundaryReason::KeyframeSearch;
    CHECK(!evaluatePlanCoverage(headless, policy).passed);

    // ... and so is a plan that does not reach the end of the source.
    SplitPlan tailLess = smallPlan(source);
    tailLess.segments.back().endsAtSourceEnd = false;
    CHECK(!evaluatePlanCoverage(tailLess, policy).passed);
}
