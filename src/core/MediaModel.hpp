// MonoSect — the explicit media model.
//
// Everything MonoSect knows about a source file is described by these types.
// Nothing else in the program is allowed to guess: if a value is unknown it is
// stored as "unknown" and the planner either works around it or refuses to plan,
// instead of assuming a container-level default that might be wrong.
#pragma once

#include "core/Bytes.hpp"
#include "core/Rational.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

enum class StreamKind { Video, Audio, Subtitle, Attachment, Data, Unknown };

[[nodiscard]] const char* streamKindName(StreamKind kind) noexcept;
[[nodiscard]] StreamKind streamKindFromFfprobe(std::string_view codecType) noexcept;

/// One stream as reported by ffprobe, with "N/A" values turned into "unknown".
struct StreamModel {
    int index = -1;
    StreamKind kind = StreamKind::Unknown;
    std::string codecName;
    std::string codecLongName;
    std::string profile;
    int level = 0;
    std::string codecTag;

    // Video
    int width = 0;
    int height = 0;
    Rational sampleAspectRatio;      // 0/0 when unknown
    Rational displayAspectRatio;
    Rational averageFrameRate;       // avg_frame_rate
    Rational baseFrameRate;          // r_frame_rate
    std::string pixelFormat;
    std::optional<int> hasBFrames;
    std::optional<int> referenceFrames;
    std::optional<int> attachedPicture;   // disposition bit
    // Rotation of the display matrix ("rotate" side data), normalized to
    // 0/90/180/270.  A vertical phone clip stores 1920x1080 plus a 90 degree
    // matrix; the stream is copied as it is, but the model has to know, because
    // the *displayed* size differs from the stored one and mixing orientations in
    // one file cannot render correctly.
    int rotationDegrees = 0;
    bool rotationKnown = false;

    // Audio
    std::optional<int> sampleRate;
    std::optional<int> channels;
    std::string channelLayout;
    std::optional<int> bitsPerSample;

    // Timing.  timebase is required for the stream to be usable.
    Rational timebase;
    bool timebaseKnown = false;
    std::optional<std::int64_t> startPts;
    std::optional<Rational> startTime;
    std::optional<std::int64_t> durationTicks;
    std::optional<Rational> duration;
    std::optional<std::int64_t> bitRate;
    std::optional<std::int64_t> frameCount;

    // Metadata
    std::string language;
    std::string title;
    std::string handlerName;
    std::vector<std::pair<std::string, std::string>> tags;
    bool isDefault = false;
    bool isForced = false;

    [[nodiscard]] bool isVideo() const noexcept { return kind == StreamKind::Video; }
    /// True when the display matrix turns the picture on its side.
    [[nodiscard]] bool isQuarterTurnRotated() const noexcept {
        return rotationKnown && (rotationDegrees == 90 || rotationDegrees == 270);
    }
    /// Width as a viewer sees it (stored width unless a quarter turn is applied).
    [[nodiscard]] int displayWidth() const noexcept {
        return isQuarterTurnRotated() ? height : width;
    }
    /// Height as a viewer sees it.
    [[nodiscard]] int displayHeight() const noexcept {
        return isQuarterTurnRotated() ? width : height;
    }
    [[nodiscard]] bool isAudio() const noexcept { return kind == StreamKind::Audio; }
    [[nodiscard]] bool isTextSubtitle() const noexcept { return kind == StreamKind::Subtitle; }

    /// Short human label: "Video: h264 1920x1080 (stream 0)".
    [[nodiscard]] std::string describe() const;
    /// "h264 1920x1080 yuv420p" / "aac 48000 Hz stereo" - used in signatures and UI.
    [[nodiscard]] std::string parameterSummary() const;
};

/// Where the container duration came from.  This is shown to the user because a
/// duration limit can only be verified against a trustworthy value.
enum class DurationSource {
    Unknown,
    Container,             // format.duration from the demuxer
    StreamTimeline,        // a stream reported a duration
    FrameCountEstimate,    // nb_frames / frame rate - an estimate, marked as such
};

/// Name of the place a duration came from, for logs, JSON and the UI tooltip.
[[nodiscard]] const char* durationSourceName(DurationSource source) noexcept;
/// True when the duration is a measurement rather than an estimate.
[[nodiscard]] bool durationSourceIsMeasured(DurationSource source) noexcept;

struct DurationTruth {
    std::optional<Rational> seconds;
    DurationSource source = DurationSource::Unknown;
    bool reliable = false;               // false for estimates and for unknown durations
    std::string explanation;             // always filled in, shown in the UI tooltip

    [[nodiscard]] bool known() const noexcept { return seconds.has_value(); }
};

struct ChapterModel {
    int id = 0;
    Rational timebase;
    std::int64_t startTicks = 0;
    std::int64_t endTicks = 0;
    [[nodiscard]] Rational startSeconds() const { return TickTime{startTicks, timebase}.seconds(); }
    [[nodiscard]] Rational endSeconds() const { return TickTime{endTicks, timebase}.seconds(); }
    std::string title;
};

struct ContainerModel {
    std::string path;
    ByteCount fileSize = 0;
    std::string formatNames;      // "matroska,webm"
    std::string formatLongName;
    Rational startTime;           // container start time, often zero
    std::optional<Rational> duration;
    std::optional<ByteCount> bitRate;
    std::vector<StreamModel> streams;
    std::vector<ChapterModel> chapters;
    std::vector<std::pair<std::string, std::string>> tags;

    // True when the demuxer reports the file as a still image (a single frame);
    // such a file cannot be split into time ranges.
    bool isStillImage = false;
    // True when the file has no duration but is otherwise readable.
    bool isLiveStreamLike = false;

    [[nodiscard]] const StreamModel* videoStream() const;
    [[nodiscard]] const StreamModel* firstStreamOfKind(StreamKind kind) const;
    [[nodiscard]] std::vector<const StreamModel*> streamsOfKind(StreamKind kind) const;
    [[nodiscard]] const StreamModel* stream(int index) const;
    [[nodiscard]] int streamCount(StreamKind kind) const;
    [[nodiscard]] bool hasChapters() const noexcept { return !chapters.empty(); }

    /// Resolved duration plus an explanation of how trustworthy it is.
    [[nodiscard]] DurationTruth durationTruth() const;

    /// True when the container can hold all streams as they are copied.
    [[nodiscard]] std::string describe() const;
};

/// A judgement about whether the video stream is variable frame rate.  MonoSect
/// never guesses: it compares the average and the base frame rate and reports the
/// numbers it compared.
struct VfrAssessment {
    bool variable = false;
    Rational averageFrameRate;
    Rational baseFrameRate;
    std::string explanation;
};

/// Everything MonoSect knows about one input file.
struct MediaModel : ContainerModel {
    DurationTruth durationResolved;  // the container duration, resolved and explained
    VfrAssessment frameRate;         // filled in by the probe service
    bool durationMissing = false;    // convenience flag used by validation
    std::vector<std::string> warnings;  // things the user should know about the file
};

/// The codec parameters that must be identical for a stream copy to be a valid
/// substitute.  Comparing signatures is how MonoSect proves that a part still
/// contains the same streams - not by looking at file names.
struct StreamSignature {
    int index = -1;
    StreamKind kind = StreamKind::Unknown;
    std::string codecName;
    std::string profile;
    int level = 0;
    int width = 0;
    int height = 0;
    std::string pixelFormat;
    Rational timebase;
    std::optional<int> sampleRate;
    std::optional<int> channels;
    std::string channelLayout;
    std::optional<int> bitsPerSample;
    std::optional<int> hasBFrames;
    int rotationDegrees = 0;
    bool rotationKnown = false;

    [[nodiscard]] static StreamSignature from(const StreamModel& stream);
    [[nodiscard]] std::string describe() const;
};

/// Compares two signatures.  Returns an empty string when they are equivalent;
/// otherwise a sentence that names the difference (used verbatim as a finding).
[[nodiscard]] std::string signatureDifference(const StreamSignature& expected, const StreamSignature& actual);

/// Compares the full set of streams of two files (same order, same kinds).
[[nodiscard]] std::vector<std::string> compareSignatureSets(const std::vector<StreamSignature>& expected,
                                                           const std::vector<StreamSignature>& actual);

[[nodiscard]] std::vector<StreamSignature> signaturesOf(const ContainerModel& model);

} // namespace monosect::core
