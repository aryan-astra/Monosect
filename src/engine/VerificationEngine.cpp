#include "engine/VerificationEngine.hpp"

#include "engine/FfmpegArgs.hpp"

#include <chrono>
#include <system_error>

namespace monosect::core {

VerificationEngine::VerificationEngine(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath,
                                       VerificationPolicy policy)
    : prober_(prober), runner_(runner), ffmpegPath_(std::move(ffmpegPath)), policy_(policy) {}

Result<bool> VerificationEngine::firstVideoPacketIsKeyframe(const std::filesystem::path& file,
                                                            const CancelCheck& shouldCancel) const {
    ProcessSpec spec;
    spec.program = prober_.path();
    spec.arguments = {"-hide_banner", "-v", "error", "-print_format", "json", "-show_entries",
                      "packet=stream_index,flags,pts", "-select_streams", "v", "-read_intervals", "%+#2",
                      "-show_packets", "-i", file.string()};
    spec.captureStdout = true;
    ProcessCallbacks callbacks;
    callbacks.shouldCancel = shouldCancel;
    const ProcessResult result = runner_.run(spec, callbacks);
    if (!result.launched || result.exitCode != 0) {
        return Error{ErrorCode::VerificationFailed, "The first video packet of the written part could not be read.",
                     joinForDisplay(result.stderrTail), "The part is written again."};
    }
    auto document = parseJson(result.stdoutText);
    if (!document.has_value()) {
        return failure<bool>(document.error());
    }
    const JsonValue* packets = document.value().find("packets");
    if (packets == nullptr || !packets->isArray() || packets->items().empty()) {
        // No video packets at all: the part has no video, so a keyframe check
        // does not apply.
        return true;
    }
    const std::string flags = packets->items().front().stringOr("flags");
    return flags.find('K') != std::string::npos;
}

Result<SegmentMeasurement> VerificationEngine::measure(const std::filesystem::path& file,
                                                       const CancelCheck& shouldCancel) const {
    SegmentMeasurement measurement;
    measurement.path = file.string();

    std::error_code error;
    if (!std::filesystem::exists(file, error) || error) {
        return measurement;  // exists == false; the caller reports it as a failure
    }
    measurement.exists = true;
    measurement.fileSizeBytes = static_cast<ByteCount>(std::filesystem::file_size(file, error));
    if (error) {
        return Error{ErrorCode::VerificationFailed, "The written part's size could not be measured.",
                     file.string() + ": " + error.message(), "The part was not published."};
    }

    auto model = prober_.probe(file);
    if (!model.has_value()) {
        return measurement;  // readable == false; the error is reported by the caller
    }
    measurement.readable = true;
    measurement.signatures = signaturesOf(model.value());
    if (model.value().durationResolved.known()) {
        measurement.duration = *model.value().durationResolved.seconds;
        measurement.durationKnown = true;
    }
    auto packets = prober_.packetIndex(file);
    if (!packets.has_value()) {
        return failure<SegmentMeasurement>(packets.error());
    }
    std::optional<Rational> first;
    std::optional<Rational> last;
    for (const auto& track : packets.value().tracks()) {
        for (const auto& packet : track.packets()) {
            if (packet.isDiscard() || !packet.presentationSeconds.isValid()) {
                continue;
            }
            if (!first || compareRational(packet.presentationSeconds, *first) < 0) {
                first = packet.presentationSeconds;
            }
            if (packet.endSeconds.isValid() && (!last || compareRational(packet.endSeconds, *last) > 0)) {
                last = packet.endSeconds;
            }
        }
    }
    if (first && last) {
        const auto span = subtractRational(*last, *first);
        if (span.has_value() && (!measurement.durationKnown || compareRational(span.value(), measurement.duration) > 0)) {
            measurement.duration = span.value();
            measurement.durationKnown = true;
        }
    }

    if (policy_.requireKeyframeStart) {
        auto keyframe = firstVideoPacketIsKeyframe(file, shouldCancel);
        if (!keyframe.has_value()) {
            return failure<SegmentMeasurement>(keyframe.error());
        }
        measurement.firstVideoPacketChecked = true;
        measurement.firstVideoPacketIsKeyframe = keyframe.value();
    }

    if (policy_.decodeCheck) {
        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = decodeCheckArgs(file.string());
        spec.captureStderr = true;
        ProcessCallbacks callbacks;
        callbacks.shouldCancel = shouldCancel;
        const ProcessResult result = runner_.run(spec, callbacks);
        if (result.canceled) {
            return Error{ErrorCode::ProcessCanceled, "The decode check was cancelled."};
        }
        if (!result.launched) {
            return Error{ErrorCode::FfmpegBinaryMissing, "ffmpeg could not be started for the decode check.",
                         result.launchError, "Install the FFmpeg runtime from Settings."};
        }
        if (result.exitCode != 0) {
            return Error{ErrorCode::DecodeFailed, "The written part failed the decode check.",
                         joinForDisplay(result.stderrTail), "The part was not published."};
        } else {
            measurement.notes.push_back("the decode check passed: the part decodes from start to end");
        }
    }

    if (policy_.contentHashCheck) {
        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = streamHashArgs(file.string());
        spec.captureStdout = true;
        ProcessCallbacks callbacks;
        callbacks.shouldCancel = shouldCancel;
        const ProcessResult result = runner_.run(spec, callbacks);
        if (result.canceled) {
            return Error{ErrorCode::ProcessCanceled, "The content hash check was cancelled."};
        }
        if (result.launched && result.exitCode == 0 && !result.stdoutText.empty()) {
            measurement.contentHash = result.stdoutText;
        } else {
            return Error{ErrorCode::VerificationFailed, "The written part's stream hash could not be computed.",
                         joinForDisplay(result.stderrTail), "The part was not published."};
        }
    }

    return measurement;
}

Result<VerificationResult> VerificationEngine::verifyPart(const SplitPlan& plan, const SegmentPlan& segment,
                                                          const std::filesystem::path& file,
                                                          std::vector<SegmentMeasurement>* measurements,
                                                          const CancelCheck& shouldCancel) const {
    auto measurement = measure(file, shouldCancel);
    if (!measurement.has_value()) {
        return failure<VerificationResult>(measurement.error());
    }

    // Content comparison: for a pure copy the payload must be identical to the
    // source range with the same boundaries.  This is the strongest statement
    // MonoSect can make, so it is only made when the check is enabled and the
    // mode really copies everything.
    if (policy_.contentHashCheck && segment.method == SegmentMethod::Copy &&
        measurement.value().contentHash.has_value()) {
        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = rangeStreamHashArgs(plan.sourcePath, segment.startSeconds, segment.endSeconds);
        spec.captureStdout = true;
        ProcessCallbacks callbacks;
        callbacks.shouldCancel = shouldCancel;
        const ProcessResult result = runner_.run(spec, callbacks);
        if (result.canceled) {
            return Error{ErrorCode::ProcessCanceled, "The source hash check was cancelled."};
        }
        if (result.launched && result.exitCode == 0 && !result.stdoutText.empty()) {
            if (result.stdoutText != *measurement.value().contentHash) {
                return Error{ErrorCode::VerificationFailed, "The part's stream hash differs from the source range.",
                             file.string(), "The part was not published."};
            } else {
                measurement.value().notes.push_back(
                    "the stream hash matches the same range of the source: the part carries the same packets");
            }
        } else {
            return Error{ErrorCode::VerificationFailed, "The source range's stream hash could not be computed.",
                         joinForDisplay(result.stderrTail), "The part was not published."};
        }
    }

    VerificationResult result = evaluateSegment(plan, segment, measurement.value(), policy_);
    if (measurements != nullptr) {
        measurements->push_back(measurement.value());
    }
    return result;
}

Result<VerificationResult> VerificationEngine::verifySequence(
    const SplitPlan& plan, const std::vector<SegmentMeasurement>& measurements) const {
    return evaluateSequence(plan, measurements, policy_);
}

} // namespace monosect::core
