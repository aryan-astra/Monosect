#include "core/SplitPlan.hpp"

#include "core/Json.hpp"
#include "core/ProbeParsing.hpp"

#include <algorithm>

namespace monosect::core {
namespace {

JsonValue rationalJson(const Rational& value) {
    JsonValue object = JsonValue::makeObject();
    object.set("num", JsonValue::makeInt(value.numerator));
    object.set("den", JsonValue::makeInt(value.denominator));
    return object;
}

} // namespace

const char* boundaryReasonName(BoundaryReason reason) noexcept {
    switch (reason) {
        case BoundaryReason::SourceStart: return "start of file";
        case BoundaryReason::SourceEnd: return "end of file";
        case BoundaryReason::ExactRequest: return "exact requested position";
        case BoundaryReason::KeyframeSearch: return "closest keyframe";
        case BoundaryReason::SizeLimit: return "size limit";
        case BoundaryReason::DurationLimit: return "duration limit";
        case BoundaryReason::TargetDuration: return "target part duration";
        case BoundaryReason::SplitPosition: return "requested split position";
        case BoundaryReason::MinimumPartSize: return "minimum part size";
        case BoundaryReason::SafetyMargin: return "safety correction";
        case BoundaryReason::RemainingSource: return "remaining source";
    }
    return "unknown";
}

const char* segmentMethodName(SegmentMethod method) noexcept {
    switch (method) {
        case SegmentMethod::Copy: return "copy";
        case SegmentMethod::SmartCut: return "smart cut";
        case SegmentMethod::ReEncode: return "re-encode";
    }
    return "unknown";
}

const SegmentPlan* SplitPlan::segment(int index) const {
    for (const auto& candidate : segments) {
        if (candidate.index == index) {
            return &candidate;
        }
    }
    return nullptr;
}

ByteCount SplitPlan::largestPredictedBytes() const {
    ByteCount largest = 0;
    for (const auto& segment : segments) {
        largest = std::max(largest, segment.predictedFileBytes);
    }
    return largest;
}

Rational SplitPlan::longestPredictedDuration() const {
    Rational longest{0, 1};
    for (const auto& segment : segments) {
        if (compareRational(segment.predictedDuration, longest) > 0) {
            longest = segment.predictedDuration;
        }
    }
    return longest;
}

bool SplitPlan::respectsHardSizeLimit() const {
    if (!constraint.hasSizeLimit()) {
        return true;
    }
    for (const auto& segment : segments) {
        if (segment.predictedFileBytes > constraint.size->maxBytes) {
            return false;
        }
    }
    return true;
}

bool SplitPlan::respectsHardDurationLimit() const {
    if (!constraint.hasDurationLimit()) {
        return true;
    }
    for (const auto& segment : segments) {
        if (compareRational(segment.predictedDuration, constraint.duration->maxSeconds) > 0) {
            return false;
        }
    }
    return true;
}

std::string SplitPlan::describe() const {
    std::string text;
    if (!planned) {
        return "The split could not be planned: " + failureExplanation;
    }
    text += "Plan for " + sourcePath + "\n";
    text += "  source: " + formatBytesShort(sourceFileBytes) + ", " +
            (sourceDurationKnown ? formatDuration(sourceDuration) : std::string("duration unknown")) + ", " +
            friendlyFormatName(sourceFormatNames) + "\n";
    text += "  parts: " + std::to_string(segments.size()) + ", " + describeConstraint(constraint) + "\n";
    for (const auto& segment : segments) {
        text += "  part " + std::to_string(segment.index) + "/" + std::to_string(segment.total) + ": " +
                formatDuration(segment.startSeconds) + " -> " + formatDuration(segment.endSeconds) +
                " (" + std::string(segmentMethodName(segment.method)) + ")";
        text += ", predicted " + formatBytesShort(segment.predictedFileBytes);
        if (constraint.hasSizeLimit()) {
            const ByteCount slack = constraint.size->maxBytes - segment.predictedFileBytes;
            text += " (" + formatBytesShort(slack > 0 ? slack : 0) + " under the limit)";
        }
        text += "\n";
        if (!segment.startBoundary.exact && segment.startBoundary.requested) {
            text += "      boundary: " + segment.startBoundary.explanation + "\n";
        }
    }
    return text;
}

std::int64_t SegmentPlan::expectedPacketCount(int streamIndex) const {
    for (const StreamPacketCount& entry : expectedPackets) {
        if (entry.streamIndex == streamIndex) {
            return entry.packetCount;
        }
    }
    return -1;
}

std::string SplitPlan::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root.set("planned", JsonValue::makeBool(planned));
    if (!planned) {
        root.set("failure", JsonValue::makeString(failureExplanation));
    }
    root.set("source_path", JsonValue::makeString(sourcePath));
    root.set("source_bytes", JsonValue::makeInt(sourceFileBytes));
    root.set("source_format", JsonValue::makeString(sourceFormatNames));
    root.set("source_duration", rationalJson(sourceDuration));
    root.set("source_duration_known", JsonValue::makeBool(sourceDurationKnown));
    root.set("output_directory", JsonValue::makeString(outputDirectory));
    root.set("output_extension", JsonValue::makeString(outputExtension));
    root.set("predicted_total_bytes", JsonValue::makeInt(predictedTotalBytes));
    root.set("size_model", JsonValue::makeString(sizeModel.toJson()));
    root.set("constraint", JsonValue::makeString(describeConstraint(constraint)));

    JsonValue segmentsJson = JsonValue::makeArray();
    for (const auto& segment : segments) {
        JsonValue item = JsonValue::makeObject();
        item.set("index", JsonValue::makeInt(segment.index));
        item.set("total", JsonValue::makeInt(segment.total));
        item.set("start", rationalJson(segment.startSeconds));
        item.set("end", rationalJson(segment.endSeconds));
        item.set("method", JsonValue::makeString(segmentMethodName(segment.method)));
        item.set("predicted_bytes", JsonValue::makeInt(segment.predictedFileBytes));
        item.set("predicted_payload_bytes", JsonValue::makeInt(segment.predictedPayloadBytes));
        item.set("predicted_duration", rationalJson(segment.predictedDuration));
        item.set("file_name", JsonValue::makeString(segment.outputFileName));
        item.set("boundary_reason", JsonValue::makeString(boundaryReasonName(segment.startBoundary.reason)));
        item.set("boundary_requested", JsonValue::makeBool(segment.startBoundary.requested));
        item.set("boundary_exact", JsonValue::makeBool(segment.startBoundary.exact));
        item.set("boundary_requested_seconds", rationalJson(segment.startBoundary.requestedSeconds));
        item.set("boundary_actual_seconds", rationalJson(segment.startBoundary.actualSeconds));
        item.set("boundary_explanation", JsonValue::makeString(segment.startBoundary.explanation));
        JsonValue packetCounts = JsonValue::makeArray();
        for (const StreamPacketCount& entry : segment.expectedPackets) {
            JsonValue count = JsonValue::makeObject();
            count.set("stream_index", JsonValue::makeInt(entry.streamIndex));
            count.set("packets", JsonValue::makeInt(entry.packetCount));
            count.set("copied", JsonValue::makeBool(entry.copied));
            packetCounts.push(std::move(count));
        }
        item.set("expected_packets", std::move(packetCounts));
        JsonValue notes = JsonValue::makeArray();
        for (const auto& note : segment.notes) {
            notes.push(JsonValue::makeString(note));
        }
        item.set("notes", std::move(notes));
        segmentsJson.push(std::move(item));
    }
    root.set("segments", std::move(segmentsJson));

    JsonValue warningsJson = JsonValue::makeArray();
    for (const std::string& warning : this->warnings) {
        warningsJson.push(JsonValue::makeString(warning));
    }
    root.set("warnings", std::move(warningsJson));
    return root.dump();
}

} // namespace monosect::core
