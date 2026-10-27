#include "TestSupport.hpp"
#include "engine/JobStore.hpp"
#include "engine/VerificationEngine.hpp"

#include <algorithm>
#include <fstream>

using namespace monosect::core;

namespace {
struct TemporaryPart {
    std::filesystem::path directory = std::filesystem::temp_directory_path() / makeJobId();
    std::filesystem::path file = directory / "part.wav";
    TemporaryPart() {
        std::filesystem::create_directory(directory);
        std::ofstream(file).put('x');
    }
    ~TemporaryPart() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

class VerificationRunner final : public IProcessRunner {
public:
    int calls = 0;
    int checkExitCode = 0;
    bool cancel = false;
    bool emptyHash = false;
    bool mismatch = false;
    int packetDuration = 48000;
    ProcessResult run(const ProcessSpec& spec, const ProcessCallbacks&) override {
        ++calls;
        ProcessResult result;
        result.launched = true;
        result.exitCode = 0;
        if (spec.program == "ffprobe") {
            result.stdoutText = R"({"format":{"format_name":"wav","duration":"1"},"streams":[{"index":0,"codec_type":"audio","codec_name":"pcm_s16le","time_base":"1/48000","sample_rate":"48000","channels":1}]})";
            if (std::find(spec.arguments.begin(), spec.arguments.end(), "-show_packets") != spec.arguments.end()) {
                result.stdoutText = "{\"packets\":[{\"stream_index\":0,\"pts\":0,\"duration\":" +
                                    std::to_string(packetDuration) + ",\"size\":1,\"pos\":0}]}";
            }
        } else {
            result.exitCode = checkExitCode;
            result.canceled = cancel;
            const bool source = std::find(spec.arguments.begin(), spec.arguments.end(), "source.wav") != spec.arguments.end();
            result.stdoutText = emptyHash ? "" : (source && mismatch ? "different" : "same");
        }
        return result;
    }
    void terminateAll() override {}
};
}

MONOSECT_TEST(an_unmeasurable_part_size_is_an_error_before_probing) {
    TemporaryPart part;
    VerificationRunner runner;
    FFprobeService probe(runner, "ffprobe");
    VerificationEngine verifier(probe, runner, "ffmpeg", {});
    CHECK(!verifier.measure(part.directory).has_value());
    CHECK_EQ(0, runner.calls);
}

MONOSECT_TEST(a_requested_decode_failure_or_cancellation_cannot_verify) {
    TemporaryPart part;
    VerificationRunner runner;
    FFprobeService probe(runner, "ffprobe");
    VerificationPolicy policy;
    policy.requireKeyframeStart = false;
    policy.decodeCheck = true;
    VerificationEngine verifier(probe, runner, "ffmpeg", policy);
    runner.checkExitCode = 1;
    const auto failed = verifier.measure(part.file);
    CHECK(!failed.has_value());
    CHECK(failed.error().code == ErrorCode::DecodeFailed);
    runner.cancel = true;
    const auto cancelled = verifier.measure(part.file);
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().code == ErrorCode::ProcessCanceled);
    runner.cancel = false;
    runner.checkExitCode = 0;
    CHECK(verifier.measure(part.file).has_value());
}

MONOSECT_TEST(a_requested_hash_must_exist_and_match_even_on_the_last_part) {
    TemporaryPart part;
    VerificationRunner runner;
    FFprobeService probe(runner, "ffprobe");
    VerificationPolicy policy;
    policy.requireKeyframeStart = false;
    policy.checkStreamSignatures = false;
    policy.contentHashCheck = true;
    VerificationEngine verifier(probe, runner, "ffmpeg", policy);
    SplitPlan plan;
    plan.sourcePath = "source.wav";
    SegmentPlan segment;
    segment.method = SegmentMethod::Copy;
    segment.endSeconds = Rational{1, 1};
    segment.endsAtSourceEnd = true;
    runner.emptyHash = true;
    CHECK(!verifier.verifyPart(plan, segment, part.file, nullptr).has_value());
    runner.emptyHash = false;
    runner.mismatch = true;
    CHECK(!verifier.verifyPart(plan, segment, part.file, nullptr).has_value());
    runner.mismatch = false;
    const auto checked = verifier.verifyPart(plan, segment, part.file, nullptr);
    CHECK(checked.has_value());
    CHECK(checked.value().passed);
}

MONOSECT_TEST(packet_duration_cannot_be_hidden_by_shorter_container_metadata) {
    TemporaryPart part;
    VerificationRunner runner;
    runner.packetDuration = 96000;
    FFprobeService probe(runner, "ffprobe");
    VerificationPolicy policy;
    policy.requireKeyframeStart = false;
    VerificationEngine verifier(probe, runner, "ffmpeg", policy);
    const auto measured = verifier.measure(part.file);
    CHECK(measured.has_value());
    CHECK(measured.value().durationKnown);
    CHECK_EQ((Rational{2, 1}), measured.value().duration);
}
