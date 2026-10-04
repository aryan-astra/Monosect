// MonoSect — the constraint contract (TODO 016-030, 061-070).
#include "TestSupport.hpp"
#include "Fixtures.hpp"

#include "core/Constraints.hpp"

using namespace monosect::core;
using namespace monosect::test;

MONOSECT_TEST(a_size_limit_that_no_part_can_meet_is_refused_before_any_work) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 1024, .minBytes = std::nullopt};   // 1 KiB for a 1 MB source
    const ConstraintReport report = validateConstraint(constraint, source.model);
    CHECK(!report.valid);
    CHECK(!report.issues.empty());
    CHECK(!report.issues.front().message.empty());
    CHECK(!report.issues.front().remedy.empty());
}

MONOSECT_TEST(a_duration_limit_below_one_packet_is_warned_about) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.duration = DurationLimit{Rational{1, 10}};   // 100 ms, less than one frame
    const ConstraintReport report = validateConstraint(constraint, source.model);
    // Validation is about the constraint itself; whether a part can be *planned*
    // inside it depends on the source's keyframes, which the planner decides (and
    // refuses, with the smallest possible part quoted).  What must not happen is
    // silence: the user is told the limit leaves almost no room.
    CHECK(report.valid);
    bool warned = false;
    for (const ConstraintIssue& issue : report.issues) {
        if (issue.severity == ConstraintIssue::Severity::Warning) {
            warned = true;
        }
    }
    CHECK(warned);
}

MONOSECT_TEST(a_reasonable_constraint_is_accepted_and_described) {
    const SyntheticSource source = makeSyntheticSource();
    UserConstraint constraint;
    constraint.size = SizeLimit{.maxBytes = 120000, .minBytes = std::nullopt};
    constraint.preservation = PreservationMode::KeyframeAligned;
    constraint.precision = PrecisionPolicy::Balanced;
    const ConstraintReport report = validateConstraint(constraint, source.model);
    CHECK(report.valid);
    const std::string description = describeConstraint(constraint);
    CHECK(description.find("120") != std::string::npos);
    CHECK(description.find("KiB") != std::string::npos || description.find("bytes") != std::string::npos);
    CHECK(description.find("copy") != std::string::npos);
}

MONOSECT_TEST(every_preservation_mode_states_what_it_guarantees) {
    for (PreservationMode mode : {PreservationMode::StreamCopy, PreservationMode::KeyframeAligned,
                                  PreservationMode::SmartCut, PreservationMode::FullReEncode}) {
        CHECK(std::string(preservationModeName(mode)).size() > 0);
        CHECK(std::string(preservationModeGuarantee(mode)).size() > 20);
        CHECK(preservationModeDetail(mode).size() > 20);
    }
    // The words the product is allowed to use must be true for the mode: a copy
    // mode never claims to re-encode, and the mode that does re-encode says so.
    const std::string copy = preservationModeGuarantee(PreservationMode::StreamCopy);
    CHECK(copy.find("bit for bit") != std::string::npos);
    CHECK(copy.find("re-encode") == std::string::npos);
    CHECK(std::string(preservationModeGuarantee(PreservationMode::KeyframeAligned)).find("re-encode") ==
          std::string::npos);
    CHECK(std::string(preservationModeGuarantee(PreservationMode::SmartCut)).find("re-encoded") !=
          std::string::npos);
    CHECK(std::string(preservationModeGuarantee(PreservationMode::FullReEncode)).find("re-encoded") !=
          std::string::npos);
}

MONOSECT_TEST(codec_and_container_compatibility_is_checked) {
    CHECK(containerAcceptsCodec("mov,mp4,m4a,3gp,3g2,mj2", "h264", StreamKind::Video));
    CHECK(containerAcceptsCodec("matroska,webm", "h264", StreamKind::Video));
    CHECK(containerAcceptsCodec("matroska,webm", "hevc", StreamKind::Video));
    CHECK(!containerAcceptsCodec("webm", "h264", StreamKind::Video));   // WebM does not allow H.264
    CHECK(!containerAcceptsCodec("mov,mp4,m4a,3gp,3g2,mj2", "subrip", StreamKind::Subtitle));
    CHECK(!codecCompatibilityNote("webm", "h264", StreamKind::Video).empty());
}
