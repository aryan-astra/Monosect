// MonoSect — the UI's wording rules (TODO 275 - TODO 299).
//
// These tests exist because the most damaging possible bug in MonoSect is not a
// crash: it is the interface claiming something the engine did not do.  Every
// check here is about what the user is told.
#include "TestSupport.hpp"
#include "Fixtures.hpp"

#include "core/SizeModel.hpp"
#include "core/SplitPlanner.hpp"

#include "ui/HelpContent.hpp"
#include "ui/UiModel.hpp"

#include <algorithm>

using namespace monosect::core;
using namespace monosect::test;

namespace {

SplitPlan plannedSplit(const SyntheticSource& source, ByteCount limit) {
    SizeModel sizeModel;
    sizeModel.seedFromSource(source.model.formatNames, source.model.fileSize, source.index.totalPayloadBytes());
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = limit, .minBytes = std::nullopt};
    constraint.preservation = PreservationMode::KeyframeAligned;
    NamingPolicy naming;
    SplitPlanner planner(source.model, source.index, constraint, naming, sizeModel);
    auto plan = planner.plan("out");
    CHECK(plan.has_value());
    return plan.has_value() ? plan.value() : SplitPlan{};
}

} // namespace

MONOSECT_TEST(the_help_covers_every_concept_the_ui_shows) {
    // Every help topic has enough text to be useful and a stable id.
    for (const HelpTopic& topic : HelpContent::topics()) {
        CHECK(!topic.idText.empty());
        CHECK(topic.title.size() > 3);
        CHECK(topic.oneLine.size() > 40);
        CHECK(topic.body.size() > 200);
        CHECK(!topic.example.empty());
        CHECK(HelpContent::findByText(topic.idText) == &topic);
    }
    CHECK(HelpContent::topics().size() >= 15);

    // Every cross-reference resolves: a "see also" entry is shown as a link, and
    // a link to nothing is a dead end in the interface.
    for (const HelpTopic& topic : HelpContent::topics()) {
        for (const std::string& other : topic.seeAlso) {
            CHECK(HelpContent::findByText(other) != nullptr);
        }
    }
    CHECK(HelpContent::find(HelpTopicId::SizeLimit) != nullptr);
    CHECK(HelpContent::find(HelpTopicId::Verification) != nullptr);

    // The sentences that must never appear, because the engine cannot deliver
    // them, must not appear in any help text.
    for (const HelpTopic& topic : HelpContent::topics()) {
        const std::string all = topic.title + topic.oneLine + topic.body + topic.example;
        CHECK(all.find("frame accurate") == std::string::npos);
        CHECK(all.find("frame-accurate") == std::string::npos);
        CHECK(all.find("guaranteed lossless") == std::string::npos);
    }

    // The help says what MonoSect will NOT do, in the topics where that matters.
    const HelpTopic* copy = HelpContent::find(HelpTopicId::StreamCopy);
    CHECK(copy != nullptr);
    if (copy != nullptr) {
        CHECK(copy->body.find("cannot cut between") != std::string::npos);
    }
}

MONOSECT_TEST(help_search_finds_topics_by_their_words) {
    const auto limit = HelpContent::search("hard limit");
    CHECK(limit.size() >= 2);
    const auto nothing = HelpContent::search("zzzz-nothing-matches-this");
    CHECK(nothing.empty());
    const auto everything = HelpContent::search("");
    CHECK_EQ(HelpContent::topics().size(), everything.size());

    // A search is case-insensitive, because the box does not shout.
    CHECK(!HelpContent::search("KEYFRAME").empty());
}

MONOSECT_TEST(the_limit_is_shown_as_exact_bytes) {
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 2 * kGiB, .minBytes = std::nullopt};
    const std::string explanation = UiModel::limitExplanation(constraint);
    CHECK(explanation.find("2,147,483,648") != std::string::npos);
    const std::string headline = UiModel::constraintHeadline(constraint);
    CHECK(headline.find("2.00 GiB") != std::string::npos);
    CHECK(headline.find("keyframe") != std::string::npos || headline.find("Keyframe") != std::string::npos);

    // A duration-only constraint must not claim a size limit exists.
    UserConstraint byDuration;
    byDuration.duration = DurationLimit{Rational{120, 1}};
    const std::string text = UiModel::limitExplanation(byDuration);
    CHECK(text.find("No size limit") != std::string::npos);
    CHECK(text.find("2,147,483,648") == std::string::npos);
}

MONOSECT_TEST(exact_boundaries_are_only_claimed_when_they_are_exact) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = plannedSplit(source, 200000);
    CHECK(plan.planned);
    CHECK(!plan.segments.empty());

    // The first part always starts at the beginning of the file.
    const auto lines = UiModel::partLines(plan);
    CHECK_EQ(plan.segments.size(), lines.size());
    if (!lines.empty()) {
        CHECK(lines.front().boundary.find("start of the file") != std::string::npos);
    }

    SegmentPlan moved = plan.segments.front();
    moved.startBoundary.exact = true;
    moved.startBoundary.requested = true;
    moved.startBoundary.requestedSeconds = Rational{4, 1};
    moved.startBoundary.actualSeconds = Rational{3, 1};
    moved.startSeconds = Rational{3, 1};
    CHECK(!UiModel::boundaryIsExact(moved));   // requested != actual, so not exact

    moved.startBoundary.actualSeconds = Rational{4, 1};
    moved.startSeconds = Rational{4, 1};
    CHECK(UiModel::boundaryIsExact(moved));
}

MONOSECT_TEST(the_plan_summary_states_what_is_estimated_and_what_is_measured) {
    const SyntheticSource source = makeSyntheticSource();
    const SplitPlan plan = plannedSplit(source, 200000);
    const std::string summary = UiModel::planSummary(plan);
    CHECK(summary.find("part(s)") != std::string::npos);
    CHECK(summary.find("limit") != std::string::npos);
    // Before any part is written, the sizes are estimates and the summary says so.
    CHECK(summary.find("estimate") != std::string::npos);

    SplitPlan failed = plan;
    failed.planned = false;
    failed.failureExplanation = "no usable boundary";
    CHECK(UiModel::planSummary(failed).find("no usable boundary") != std::string::npos);
}

MONOSECT_TEST(re_encode_regions_are_disclosed_not_hidden) {
    const SyntheticSource source = makeSyntheticSource();
    SplitPlan plan = plannedSplit(source, 200000);
    plan.segments.front().method = SegmentMethod::SmartCut;
    ReencodeRegion region;
    region.startSeconds = Rational{0, 1};
    region.endSeconds = Rational{1, 1};
    region.reason = "the cut is not on a keyframe";
    plan.segments.front().reencodeRegions.push_back(region);
    const auto caveats = UiModel::planCaveats(plan);
    bool disclosed = false;
    for (const std::string& line : caveats) {
        if (line.find("re-encoded") != std::string::npos && line.find("0:01.000") != std::string::npos) {
            disclosed = true;
        }
    }
    CHECK(disclosed);
}

MONOSECT_TEST(verification_wording_matches_the_result) {
    VerificationResult passed;
    passed.passed = true;
    passed.summary = "part 1: verified";
    JobStatusLine line = UiModel::verificationLine(passed);
    CHECK_EQ(std::string("Verified"), line.headline);
    CHECK(line.tone == StatusTone::Good);

    VerificationResult failed;
    failed.passed = false;
    failed.summary = "part 1: not verified";
    VerificationFinding finding;
    finding.severity = FindingSeverity::Error;
    finding.message = "The part is 1 byte over the limit.";
    failed.findings.push_back(finding);
    line = UiModel::verificationLine(failed);
    CHECK_EQ(std::string("Not verified"), line.headline);
    CHECK(line.tone == StatusTone::Bad);
    CHECK(line.detail.find("1 byte over") != std::string::npos);

    // A job that finished without a verification result is not called verified.
    line = UiModel::jobLine("wrote 3 part(s)", true, false, false);
    CHECK(line.headline.find("not verified") != std::string::npos);
    line = UiModel::jobLine("wrote 3 part(s)", true, false, true);
    CHECK_EQ(std::string("Finished and verified"), line.headline);
    line = UiModel::jobLine("cancelled after part 2", false, true, true);
    CHECK_EQ(std::string("Cancelled"), line.headline);
    line = UiModel::jobLine("the limit cannot be met", false, false, false);
    CHECK_EQ(std::string("Failed"), line.headline);
}

MONOSECT_TEST(metrics_are_never_invented) {
    // An unmeasurable metric is "n/a" - never zero, because a zero is a claim.
    const MeasuredValue unknown = MeasuredValue::unknown("no GPU telemetry interface");
    CHECK_EQ(std::string("n/a"), percentText(unknown));
    CHECK_EQ(std::string("n/a"), bytesPerSecondText(unknown));
    CHECK_EQ(std::string("n/a"), UiModel::meterText(unknown, "MB/s", 1));

    const MeasuredValue known = MeasuredValue::of(42.5);
    CHECK_EQ(std::string("42%"), percentText(known));
    CHECK(UiModel::meterText(known, "MB/s", 1).find("42.5") != std::string::npos);
}

MONOSECT_TEST(an_eta_is_only_offered_when_it_can_be_computed) {
    const ProgressView finished = UiModel::progress(5, 5, 5000000.0, 30.0);
    CHECK(!finished.secondsRemaining.has_value());
    CHECK(finished.etaExplanation.empty());
    CHECK(finished.describe().find("remaining") == std::string::npos);
    const ProgressView none = UiModel::progress(0, 5, 0.0, std::nullopt);
    CHECK(!none.secondsRemaining.has_value());
    const std::string text = none.describe();
    CHECK(text.find("no estimate") != std::string::npos);
    CHECK(text.find("0 of 5") != std::string::npos);

    const ProgressView known = UiModel::progress(2, 5, 5000000.0, 30.0);
    CHECK(known.secondsRemaining.has_value());
    CHECK_NEAR(0.4, known.fraction, 1e-9);
    CHECK(known.describe().find("0:30.000") != std::string::npos);
    CHECK(known.describe().find("no estimate") == std::string::npos);

    const ProgressView notStarted = UiModel::progress(0, 5, 0.0, 12.0);
    CHECK(notStarted.secondsRemaining.has_value());   // the caller supplied a measured value

    const ProgressView empty = UiModel::progress(0, 0, 0.0, std::nullopt);
    CHECK_NEAR(0.0, empty.fraction, 1e-9);
}

MONOSECT_TEST(the_resource_policy_is_described_including_its_reason) {
    ResourcePolicyInput input;
    input.mode = ResourceMode::Maximum;
    input.topology.logicalCores = 8;
    input.topology.physicalCores = 4;
    const ResourcePolicy policy = deriveResourcePolicy(input);
    const std::string text = UiModel::resourcePolicySummary(policy);
    CHECK(text.find(resourceModeName(ResourceMode::Maximum)) != std::string::npos);
    CHECK(text.find("thread") != std::string::npos);
    CHECK(text.size() > 60);   // the rationale is always present
}
