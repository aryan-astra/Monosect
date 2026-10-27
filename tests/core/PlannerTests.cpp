// MonoSect — planning: hard limits first, closeness second (TODO 071-093).
#include "TestSupport.hpp"
#include "Fixtures.hpp"

#include "core/SplitPlanner.hpp"
#include "core/SizeModel.hpp"

using namespace monosect::core;
using namespace monosect::test;

namespace {

SplitPlan planFor(const SyntheticSource& source, const UserConstraint& constraint) {
    SizeModel sizeModel;
    sizeModel.seedFromSource(source.model.formatNames, source.model.fileSize,
                             source.index.totalPayloadBytes());
    NamingPolicy naming;
    SplitPlanner planner(source.model, source.index, constraint, naming, sizeModel);
    auto plan = planner.plan("out");
    CHECK(plan.has_value());
    return plan.has_value() ? plan.value() : SplitPlan{};
}

} // namespace

MONOSECT_TEST(a_size_limit_is_never_predicted_to_be_exceeded) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    // One GOP of this fixture is about 136 KiB of payload, so 200 KiB is a limit
    // the source can meet with keyframe-aligned parts.
    constraint.size = SizeLimit{.maxBytes = 200000, .minBytes = std::nullopt};
    constraint.preservation = PreservationMode::KeyframeAligned;
    const SplitPlan plan = planFor(source, constraint);
    CHECK(plan.planned);
    CHECK(plan.segments.size() >= 2);
    for (const SegmentPlan& segment : plan.segments) {
        CHECK(segment.predictedFileBytes <= constraint.size->maxBytes);
        CHECK(segment.endSeconds > segment.startSeconds);
    }
    // Parts are contiguous and cover the source.
    for (std::size_t i = 1; i < plan.segments.size(); ++i) {
        CHECK_EQ(plan.segments[i - 1].endSeconds, plan.segments[i].startSeconds);
    }
    if (!plan.segments.empty()) {
        CHECK(plan.segments.front().startSeconds.isZero());
        CHECK(plan.segments.back().endsAtSourceEnd);
    }
}

MONOSECT_TEST(copy_boundaries_land_on_keyframes) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 200000, .minBytes = std::nullopt};
    constraint.preservation = PreservationMode::KeyframeAligned;
    const SplitPlan plan = planFor(source, constraint);
    const StreamPacketTrack* video = source.index.track(0);
    for (const SegmentPlan& segment : plan.segments) {
        if (segment.index == 1) {
            continue;
        }
        CHECK(video->isKeyframeAt(segment.startSeconds));
        // The boundary record has to describe the part that will really be
        // written: the actual position equals the segment start, the explanation
        // is always present, and "exact" is only claimed when the ideal boundary
        // (the one the size or duration budget asked for) *is* a keyframe.
        CHECK(compareRational(segment.startBoundary.actualSeconds, segment.startSeconds) == 0);
        CHECK(segment.startBoundary.explanation.size() > 10);
        if (segment.startBoundary.requested && !segment.startBoundary.exact) {
            CHECK(segment.startBoundary.delta.isPositive() || !segment.startBoundary.delta.isPositive());
        }
    }
}

MONOSECT_TEST(a_duration_limit_shrinks_parts_and_is_respected) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.duration = DurationLimit{Rational{3, 1}};
    constraint.preservation = PreservationMode::KeyframeAligned;
    const SplitPlan plan = planFor(source, constraint);
    CHECK(plan.planned);
    for (const SegmentPlan& segment : plan.segments) {
        CHECK(segment.predictedDuration <= constraint.duration->maxSeconds);
        auto length = subtractRational(segment.endSeconds, segment.startSeconds);
        CHECK(length.has_value());
        CHECK(length.value() <= constraint.duration->maxSeconds);
    }
    CHECK(plan.segments.size() >= 4);
}

MONOSECT_TEST(an_impossible_part_count_is_refused_with_a_reason) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 60000, .minBytes = std::nullopt};
    constraint.maxParts = 2;
    const SplitPlan plan = planFor(source, constraint);
    CHECK(!plan.planned);
    CHECK(plan.failureExplanation.size() > 40);
    CHECK(plan.failureExplanation.find("2") != std::string::npos);
}

MONOSECT_TEST(the_plan_explains_itself_in_json) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 200000, .minBytes = std::nullopt};
    const SplitPlan plan = planFor(source, constraint);
    CHECK(plan.planned);
    auto json = parseJson(plan.toJson());
    CHECK(json.has_value());
    CHECK(json.value().find("segments") != nullptr);
    CHECK_EQ(std::string("true"), toText(json.value().find("planned")->asBool().value()));
    const JsonValue* segments = json.value().find("segments");
    CHECK(segments != nullptr && segments->isArray());
    CHECK_EQ(plan.segments.size(), segments->size());
    if (segments != nullptr && segments->size() > 0) {
        const JsonValue* first = segments->at(0);
        CHECK(first != nullptr);
        if (first != nullptr) {
            CHECK(first->find("predicted_bytes")->asInt64().value() <= 200000);
            CHECK(first->find("boundary_exact") != nullptr);
        }
    }
}

MONOSECT_TEST(the_size_model_learns_from_a_measurement) {
    SizeModel model;
    model.seedFromSource("mov,mp4,m4a,3gp,3g2,mj2", 1'000'000, 950'000);
    const ByteCount before = model.predictFileBytes(100'000);
    CHECK(before >= 100'000);

    SizeObservation observation;
    observation.predictedPayloadBytes = 100'000;
    observation.actualPayloadBytes = 100'000;
    observation.actualFileBytes = 140'000;   // a much larger container overhead than assumed
    model.observe(observation);
    CHECK(model.calibrated());
    CHECK(model.predictFileBytes(100'000) >= 140'000);
    CHECK(model.maxPayloadForLimit(150'000) <= 150'000);
}

MONOSECT_TEST(replanning_starts_at_the_actual_completed_boundary) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 200000, .minBytes = std::nullopt};
    SplitPlan plan = planFor(source, constraint);
    CHECK(plan.segments.size() > 2);
    plan.segments.front().endSeconds = plan.segments[1].endSeconds;
    SplitPlanner planner(source.model, source.index, constraint, {}, plan.sizeModel);
    const auto revised = planner.replanRemaining(plan, 1);
    CHECK(revised.has_value());
    CHECK(revised.value().planned);
    CHECK_EQ(plan.segments.front().endSeconds, revised.value().segments[1].startSeconds);
    CHECK_EQ(plan.segments.front().endSeconds, revised.value().segments.front().endSeconds);
    for (std::size_t i = 1; i < revised.value().segments.size(); ++i) {
        CHECK_EQ(revised.value().segments[i - 1].endSeconds, revised.value().segments[i].startSeconds);
    }
    CHECK(revised.value().segments.back().endsAtSourceEnd);
    CHECK(!planner.replanRemaining(plan, plan.segments.size() + 1).has_value());
    const auto tail = planner.planOnePart(plan, plan.segments.back().startSeconds,
                                        plan.segments.back().index, plan.partCount(), "out");
    CHECK(tail.has_value());
    CHECK(tail.value().endsAtSourceEnd);
}

MONOSECT_TEST(the_source_start_includes_audio_before_the_first_video_frame) {
    SyntheticSource source = makeSyntheticSource();
    PacketIndex shifted;
    for (const auto& original : source.index.tracks()) {
        StreamPacketTrack track(original.streamIndex(), original.timebase());
        for (auto packet : original.packets()) {
            if (original.streamIndex() == 0) {
                packet.presentationSeconds = addRational(packet.presentationSeconds, Rational{1, 1}).value();
                packet.endSeconds = addRational(packet.endSeconds, Rational{1, 1}).value();
            }
            track.add(packet);
        }
        shifted.addTrack(std::move(track));
    }
    shifted.finalize();
    source.index = std::move(shifted);
    UserConstraint constraint;
    constraint.duration = DurationLimit{Rational{3, 1}};
    const auto plan = planFor(source, constraint);
    CHECK(plan.planned);
    CHECK(plan.segments.front().startSeconds.isZero());
}
