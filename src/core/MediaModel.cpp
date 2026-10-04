#include "core/MediaModel.hpp"

#include "core/Bytes.hpp"
#include "core/ProbeParsing.hpp"
#include "core/Rational.hpp"

#include <algorithm>
#include <cstdio>

namespace monosect::core {
namespace {

std::string numberOrDash(std::optional<int> value) {
    return value.has_value() ? std::to_string(*value) : std::string("unknown");
}

} // namespace

const char* streamKindName(StreamKind kind) noexcept {
    switch (kind) {
        case StreamKind::Video: return "video";
        case StreamKind::Audio: return "audio";
        case StreamKind::Subtitle: return "subtitle";
        case StreamKind::Attachment: return "attachment";
        case StreamKind::Data: return "data";
        case StreamKind::Unknown: return "unknown";
    }
    return "unknown";
}

const char* durationSourceName(DurationSource source) noexcept {
    switch (source) {
        case DurationSource::Container: return "container";
        case DurationSource::StreamTimeline: return "stream timeline";
        case DurationSource::FrameCountEstimate: return "frame count estimate";
        case DurationSource::Unknown: return "unknown";
    }
    return "unknown";
}

bool durationSourceIsMeasured(DurationSource source) noexcept {
    return source == DurationSource::Container || source == DurationSource::StreamTimeline;
}

StreamKind streamKindFromFfprobe(std::string_view codecType) noexcept {
    if (codecType == "video") return StreamKind::Video;
    if (codecType == "audio") return StreamKind::Audio;
    if (codecType == "subtitle") return StreamKind::Subtitle;
    if (codecType == "attachment") return StreamKind::Attachment;
    if (codecType == "data") return StreamKind::Data;
    return StreamKind::Unknown;
}

std::string StreamModel::describe() const {
    // Built with append() rather than chains of operator+: GCC 12 emits a
    // -Wrestrict false positive for one long concatenation, and this form is
    // also easier to read.
    std::string text(streamKindName(kind));
    text.append(" (stream ");
    text.append(std::to_string(index));
    text.append("): ");
    text.append(codecName);
    if (isVideo() && width > 0 && height > 0) {
        text.push_back(' ');
        text.append(std::to_string(width));
        text.push_back('x');
        text.append(std::to_string(height));
        if (isQuarterTurnRotated()) {
            text.append(" (displayed ");
            text.append(std::to_string(displayWidth()));
            text.push_back('x');
            text.append(std::to_string(displayHeight()));
            text.append(", rotated ");
            text.append(std::to_string(rotationDegrees));
            text.append(" degrees)");
        }
    }
    if (isAudio() && sampleRate.has_value()) {
        text.push_back(' ');
        text.append(std::to_string(*sampleRate));
        text.append(" Hz, ");
        text.append(numberOrDash(channels));
        text.append(" channel(s)");
    }
    if (!language.empty() && language != "und") {
        text.append(", language ");
        text.append(language);
    }
    return text;
}

std::string StreamModel::parameterSummary() const {
    std::string text = codecName;
    if (isVideo()) {
        if (width > 0 && height > 0) {
            text.push_back(' ');
            text.append(std::to_string(width));
            text.push_back('x');
            text.append(std::to_string(height));
        }
        if (!pixelFormat.empty()) {
            text.push_back(' ');
            text.append(pixelFormat);
        }
        if (!profile.empty()) {
            text.push_back(' ');
            text.append(profile);
        }
        if (isQuarterTurnRotated()) {
            // Stored size and displayed size differ: say both, because "320x240
            // rotated 90" is what the user sees, not what the file stores.
            text.append(" rotated ");
            text.append(std::to_string(rotationDegrees));
        }
        if (baseFrameRate.isValid() && baseFrameRate.numerator > 0) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), " %.3f fps", baseFrameRate.toDouble());
            text.append(buffer);
        }
    } else if (isAudio()) {
        if (sampleRate.has_value()) {
            text.push_back(' ');
            text.append(std::to_string(*sampleRate));
            text.append(" Hz");
        }
        if (channels.has_value()) {
            text.push_back(' ');
            text.append(std::to_string(*channels));
            text.push_back('c');
            text.push_back('h');
        }
        if (!channelLayout.empty()) {
            text.push_back(' ');
            text.append(channelLayout);
        }
    }
    return text;
}

const StreamModel* ContainerModel::stream(int index) const {
    for (const StreamModel& item : streams) {
        if (item.index == index) {
            return &item;
        }
    }
    return nullptr;
}

const StreamModel* ContainerModel::firstStreamOfKind(StreamKind kind) const {
    for (const StreamModel& item : streams) {
        if (item.kind == kind) {
            return &item;
        }
    }
    return nullptr;
}

const StreamModel* ContainerModel::videoStream() const {
    return firstStreamOfKind(StreamKind::Video);
}

std::vector<const StreamModel*> ContainerModel::streamsOfKind(StreamKind kind) const {
    std::vector<const StreamModel*> out;
    for (const StreamModel& item : streams) {
        if (item.kind == kind) {
            out.push_back(&item);
        }
    }
    return out;
}

int ContainerModel::streamCount(StreamKind kind) const {
    int count = 0;
    for (const StreamModel& item : streams) {
        if (item.kind == kind) {
            ++count;
        }
    }
    return count;
}

DurationTruth ContainerModel::durationTruth() const {
    DurationTruth truth;
    // The order is deliberate: the container's own duration is a measurement;
    // a stream's timeline is the next best thing; a frame count with a frame
    // rate is only an estimate and is labelled as one.
    if (duration.has_value() && duration->isValid() && duration->isPositive()) {
        truth.seconds = duration;
        truth.source = DurationSource::Container;
        truth.reliable = true;
        truth.explanation = "the duration the container reports";
        return truth;
    }
    for (const StreamModel& item : streams) {
        if (item.duration.has_value() && item.duration->isValid() && item.duration->isPositive()) {
            truth.seconds = item.duration;
            truth.source = DurationSource::StreamTimeline;
            truth.reliable = true;
            truth.explanation = "the duration of stream " + std::to_string(item.index) + ", which is the only " +
                                "timeline the container reports";
            return truth;
        }
        if (item.frameCount.has_value() && item.averageFrameRate.isValid() && item.averageFrameRate.numerator > 0) {
            const Rational fps = item.averageFrameRate;
            Rational estimate{*item.frameCount * fps.denominator, fps.numerator};
            if (estimate.isPositive()) {
                truth.seconds = estimate;
                truth.source = DurationSource::FrameCountEstimate;
                truth.reliable = false;
                truth.explanation = std::to_string(*item.frameCount) + " frame(s) at " + rationalToString(fps) +
                                    " - an estimate, not a measurement";
                return truth;
            }
        }
    }
    truth.source = DurationSource::Unknown;
    truth.reliable = false;
    truth.explanation = "the file does not report a duration and has no stream timeline to derive one from";
    return truth;
}

std::string ContainerModel::describe() const {
    std::string text(friendlyFormatName(*this));
    text.append(", ");
    text.append(formatBytesExact(fileSize));
    text.push_back(' ');
    text.push_back('(');
    text.append(std::to_string(streams.size()));
    text.append(" stream(s)");
    if (!chapters.empty()) {
        text.append(", ");
        text.append(std::to_string(chapters.size()));
        text.append(" chapter(s)");
    }
    text.push_back(')');
    return text;
}

StreamSignature StreamSignature::from(const StreamModel& stream) {
    StreamSignature signature;
    signature.index = stream.index;
    signature.kind = stream.kind;
    signature.codecName = stream.codecName;
    signature.profile = stream.profile;
    signature.level = stream.level;
    signature.width = stream.width;
    signature.height = stream.height;
    signature.pixelFormat = stream.pixelFormat;
    signature.timebase = stream.timebase;
    signature.sampleRate = stream.sampleRate;
    signature.channels = stream.channels;
    signature.channelLayout = stream.channelLayout;
    signature.bitsPerSample = stream.bitsPerSample;
    signature.hasBFrames = stream.hasBFrames;
    signature.rotationDegrees = stream.rotationDegrees;
    signature.rotationKnown = stream.rotationKnown;
    return signature;
}

std::string StreamSignature::describe() const {
    std::string text(streamKindName(kind));
    text.append(" #");
    text.append(std::to_string(index));
    text.push_back(' ');
    text.append(codecName);
    if (!profile.empty()) {
        text.append(" (");
        text.append(profile);
        text.push_back(')');
    }
    if (kind == StreamKind::Video) {
        text.push_back(' ');
        text.append(std::to_string(width));
        text.push_back('x');
        text.append(std::to_string(height));
        text.push_back(' ');
        text.append(pixelFormat);
        if (rotationKnown && rotationDegrees != 0) {
            text.append(", rotated ");
            text.append(std::to_string(rotationDegrees));
            text.append(" degrees");
        }
    } else if (kind == StreamKind::Audio) {
        text.push_back(' ');
        text.append(numberOrDash(sampleRate));
        text.append(" Hz ");
        text.append(numberOrDash(channels));
        text.append("ch");
    }
    return text;
}

std::string signatureDifference(const StreamSignature& expected, const StreamSignature& actual) {
    if (expected.kind != actual.kind) {
        return "stream " + std::to_string(expected.index) + " changed kind from " +
               streamKindName(expected.kind) + " to " + streamKindName(actual.kind);
    }
    if (expected.codecName != actual.codecName) {
        return "stream " + std::to_string(expected.index) + " changed codec from " + expected.codecName + " to " +
               actual.codecName;
    }
    if (!expected.profile.empty() && expected.profile != actual.profile && !actual.profile.empty()) {
        return "stream " + std::to_string(expected.index) + " changed profile from " + expected.profile + " to " +
               actual.profile;
    }
    if (expected.kind == StreamKind::Video) {
        if (expected.width != actual.width || expected.height != actual.height) {
            return "stream " + std::to_string(expected.index) + " changed dimensions from " +
                   std::to_string(expected.width) + "x" + std::to_string(expected.height) + " to " +
                   std::to_string(actual.width) + "x" + std::to_string(actual.height);
        }
        if (!expected.pixelFormat.empty() && !actual.pixelFormat.empty() &&
            expected.pixelFormat != actual.pixelFormat) {
            return "stream " + std::to_string(expected.index) + " changed pixel format from " +
                   expected.pixelFormat + " to " + actual.pixelFormat;
        }
        // A display matrix travels with the stream on a copy, so two inputs with
        // different rotations cannot be joined: one of them would be shown turned
        // the wrong way.  A missing matrix means "no rotation".
        const int expectedRotation = expected.rotationKnown ? expected.rotationDegrees : 0;
        const int actualRotation = actual.rotationKnown ? actual.rotationDegrees : 0;
        if (expectedRotation != actualRotation) {
            return "stream " + std::to_string(expected.index) + " changed display rotation from " +
                   std::to_string(expectedRotation) + " to " + std::to_string(actualRotation) +
                   " degrees, so the picture would be turned one way in one part and another way in the next";
        }
    }
    if (expected.kind == StreamKind::Audio) {
        if (expected.sampleRate != actual.sampleRate) {
            return "stream " + std::to_string(expected.index) + " changed sample rate from " +
                   numberOrDash(expected.sampleRate) + " to " + numberOrDash(actual.sampleRate);
        }
        if (expected.channels != actual.channels) {
            return "stream " + std::to_string(expected.index) + " changed channel count from " +
                   numberOrDash(expected.channels) + " to " + numberOrDash(actual.channels);
        }
    }
    return {};
}

std::vector<std::string> compareSignatureSets(const std::vector<StreamSignature>& expected,
                                              const std::vector<StreamSignature>& actual) {
    std::vector<std::string> differences;
    if (expected.size() != actual.size()) {
        differences.push_back("the file has " + std::to_string(actual.size()) + " stream(s) but " +
                              std::to_string(expected.size()) + " were expected");
    }
    const std::size_t common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        const std::string difference = signatureDifference(expected[i], actual[i]);
        if (!difference.empty()) {
            differences.push_back(difference);
        }
    }
    return differences;
}

std::vector<StreamSignature> signaturesOf(const ContainerModel& model) {
    std::vector<StreamSignature> signatures;
    signatures.reserve(model.streams.size());
    for (const auto& stream : model.streams) {
        signatures.push_back(StreamSignature::from(stream));
    }
    return signatures;
}

} // namespace monosect::core
