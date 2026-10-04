// MonoSect — FFmpeg command construction.
//
// Arguments are built as a vector of strings and handed to the process runner;
// no shell is involved, so no path or file name can be interpreted as a command.
// The functions here are pure, which makes them unit-testable without FFmpeg.
//
// Measured behaviour that these arguments rely on (see docs/RESEARCH.md and
// tests/harness/README.md):
//  * With `-ss` *before* `-i` and `-c copy`, FFmpeg seeks to the keyframe at or
//    before the requested position and keeps everything from there.  A boundary
//    that is exactly a keyframe therefore produces a part that starts exactly at
//    that keyframe - which is why the planner only places copy boundaries on
//    keyframes.
//  * When re-encoding, the same input seek is frame accurate by default, because
//    FFmpeg decodes and discards the extra frames up to the requested position.
//  * `-t` counts from that seek position; in copy mode the part's container
//    duration can reach one frame beyond the requested end, so a hard duration
//    limit is written with one frame of margin and then verified.
#pragma once

#include "core/MediaModel.hpp"
#include "core/Rational.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace monosect::core {

/// Exact seconds rendered for FFmpeg: a decimal with up to six fractional
/// digits, rounded to the nearest microsecond.  FFmpeg's time base is
/// microseconds, so this cannot lose a frame boundary.
[[nodiscard]] std::string formatSecondsForFfmpeg(const Rational& seconds);

/// Container name for `-f` and the extension MonoSect writes.
[[nodiscard]] std::string containerNameForExtension(const std::string& extension);

/// Encoder FFmpeg should use for a codec that is being re-encoded.  Returns an
/// empty string when MonoSect has no mapping (the caller then refuses to
/// re-encode rather than guessing).
[[nodiscard]] std::string encoderForCodec(std::string_view codecName, StreamKind kind);

/// Quality arguments for the mapped encoder.
[[nodiscard]] std::vector<std::string> qualityArgumentsFor(std::string_view codecName, int crf);

/// One stream of a part, as the writer has to produce it.
struct StreamTrim {
    /// Index of the stream in the *output* of the pass that carries it.  A
    /// trim pass always writes exactly one stream, so its output index is 0;
    /// the multi-stream re-encode pass uses the order of its own `-map` list.
    int outputStreamIndex = 0;
    /// Number of packets to write.  `hasPacketCount` false means "run to the end
    /// of the input" (used for the last part of a file).
    std::int64_t packetCount = 0;
    bool hasPacketCount = true;
};

struct SegmentRenderRequest {
    std::string input;
    std::string output;
    Rational start;
    Rational end;

    /// Streams that are copied unchanged.
    std::vector<int> copiedStreams;
    /// Streams that are re-encoded (smart cut heads, full re-encode), with the
    /// codec MonoSect read from the source so the encoder can match it.
    struct ReencodedStream {
        int index = -1;
        std::string codecName;
        StreamKind kind = StreamKind::Unknown;
        int bitRate = 0;              // source bit rate, used to cap the encoder
    };
    std::vector<ReencodedStream> reencodedStreams;
    /// Video/audio parameters taken from the source so the re-encoded stream
    /// stays compatible with the copied streams.
    std::string pixelFormat;
    std::string profile;
    int frameRateNumerator = 0;
    int frameRateDenominator = 0;

    std::string containerFormat;      // "matroska", "mp4", "mpegts", ...
    bool avoidNegativeTimestamps = true;
    bool copyMetadata = true;
    bool writeProgress = true;
    int crf = 18;
    int threads = 0;                  // 0 = let FFmpeg decide
    bool lowPriority = false;
    /// Exact end control for a pass that writes several streams at once.  FFmpeg
    /// stops the whole output when a `-frames` limit is reached, so a multi-stream
    /// pass may carry at most one trim; the copy path gives every stream its own
    /// trim pass instead.  An empty list means "no packet-count limit": the end is
    /// then governed by `-t` (the measured rule) and verified afterwards.
    std::vector<StreamTrim> trims;
    /// True when the start position is known to be a keyframe (copy mode).
    bool startIsKeyframe = true;
};

[[nodiscard]] std::vector<std::string> renderSegmentArgs(const SegmentRenderRequest& request);

// ---------------------------------------------------------------------------
// Exact copying.
//
// Measured with ffmpeg 7.0.2 (tests/harness/README.md records every command and
// number):
//
//  * `-ss` *before* `-i` with `-c copy` does not start where it was asked to.
//    It starts at the demuxer's seek point, which for audio can be well before
//    the requested time (0.096 s early in the mp4 fixture), and the timestamps
//    are re-based, so the error is not visible in the output.
//  * `-ss` *after* `-i` starts at the first packet whose timestamp is at or
//    after the requested time, for every stream (measured: video 2.000000 and
//    audio 2.020136 for a seek to 2 s in the mp4 fixture).
//  * `-t`/`-to` do not end a copy where they say: a part asked to end at 1.96 s
//    contained 51 packets instead of 50, and the extra packet was the keyframe
//    after the boundary.
//  * `-frames:N` (or `-frames:<type>:<index>`) stops after exactly N written
//    packets, and the stream specifier refers to *output* streams.
//  * The `-frames` limit is a stop condition for the whole output, so it may
//    only be used on one stream per FFmpeg run: with 50 video and 87 audio
//    packets requested, ffmpeg produced 48 video packets because the audio
//    limit was reached first.
//
// A copied part is therefore produced in three steps: a window that starts
// before the part (fast input seek, may start early), one trim per stream that
// starts exactly at the part's first packet and stops at exactly the planned
// packet count, and a final mux that joins the trimmed streams.  The plan
// records the packet counts and the verifier compares the content of every
// copied stream against the source range, so none of this is assumed.
// ---------------------------------------------------------------------------

/// Step 1: a window that contains the part and starts before it.  The input
/// seek is fast and may land early; step 2 removes the surplus from the start
/// and the end.
struct CopyWindowRequest {
    std::string input;
    std::string output;
    /// Where the window should start looking; the demuxer may start earlier.
    Rational requestedStart;
    /// How long the window should be.  Must cover the part plus one margin on
    /// each side, because the seek may land before `requestedStart` and the
    /// `-t` rule can stop one packet late.
    Rational length;
    /// Source stream indexes to carry in the window.
    std::vector<int> streams;
    /// Container used for the intermediate files.  Matroska is chosen because
    /// it holds every codec the source may contain; nothing is re-encoded.
    std::string containerFormat = "matroska";
    bool writeProgress = false;
    int threads = 0;
};
[[nodiscard]] std::vector<std::string> copyWindowArgs(const CopyWindowRequest& request);

/// Step 2: one stream, trimmed to exactly `packetCount` packets starting at the
/// first packet at or after `start`.
///
/// The seek form matters and is not interchangeable (measured, see above):
///  * `OutputPrecise` (`-i input -ss start`) starts exactly at the first packet
///    at or after `start` - correct for audio, subtitles and data.
///  * `InputKeyframe` (`-ss start -i input`) starts at the keyframe at or before
///    `start` - used for video, and exact precisely when `start` is a keyframe.
///    Output-seeking a video stream is not usable: FFmpeg skipped twice the seek
///    distance and relabelled the timestamps, which shifts the content silently.
struct StreamTrimRequest {
    enum class SeekMode { OutputPrecise, InputKeyframe };

    std::string input;
    std::string output;
    int inputStreamIndex = 0;
    Rational start;
    SeekMode seekMode = SeekMode::OutputPrecise;
    std::int64_t packetCount = 0;
    bool hasPacketCount = true;
    std::string containerFormat = "matroska";
    int threads = 0;
};

[[nodiscard]] std::vector<std::string> trimStreamArgs(const StreamTrimRequest& request);

/// Step 3: joins the trimmed streams into the part without re-encoding.
/// One `ffmpeg -f concat` run that joins already-compatible files.
struct MergeArgsRequest {
    std::string manifest;          // the FFconcat list
    std::string output;
    std::string containerFormat;
    bool writeProgress = true;
    int threads = 0;
};

/// Builds the concatenation command.  `-safe 0` is required because the manifest
/// lists absolute paths, which is what the concat demuxer documents.
[[nodiscard]] std::vector<std::string> mergeArgs(const MergeArgsRequest& request);

struct MuxStreamsRequest {
    std::string output;
    /// One file per stream, in the order of the part's stream list.
    std::vector<std::string> inputs;
    /// Optional extra input used only as the source of container metadata
    /// (the original file).  It contributes no streams.
    std::string metadataSource;
    std::string containerFormat;
    /// Keep every input's own timestamps instead of letting the muxer shift each
    /// input so that it starts at zero.
    ///
    /// This is required, not cosmetic.  Each trimmed stream is already relative
    /// to the *same* origin (the part's start), but a stream whose first packet
    /// does not sit exactly on that origin - audio almost never does, because an
    /// AAC packet is about 21 ms long - keeps its real distance from the origin
    /// only when the muxer is told not to re-base the inputs.  Measured on the
    /// test fixture (MP4, H.264 + AAC 44.1 kHz, part starting at 2 s): without
    /// -copyts the first audio packet landed at 0.000000, i.e. the audio was
    /// 20.136 ms early compared with the source; with -copyts it lands at
    /// 0.020000, 0.137 ms from the source's 0.020136 (the residual is the output
    /// seek's sample-grid rounding, less than one audio sample).
    bool preserveTimestamps = false;
    bool writeProgress = true;
    int threads = 0;
};
[[nodiscard]] std::vector<std::string> muxStreamsArgs(const MuxStreamsRequest& request);

/// Concatenates rendered parts without re-encoding (used by the smart cut path).
[[nodiscard]] std::vector<std::string> concatArgs(const std::string& listPath, const std::string& output,
                                                  const std::string& containerFormat);

/// Writes a concat-demuxer list.  Paths are escaped according to the demuxer's
/// documented rules (single quotes are escaped as '\'').
[[nodiscard]] Status writeConcatList(const std::filesystem::path& listPath,
                                     const std::vector<std::filesystem::path>& parts);

/// Decode check: reads the whole file and fails on the first decoding error.
[[nodiscard]] std::vector<std::string> decodeCheckArgs(const std::string& input);

/// Stream hash of a file's streams (used for the optional content comparison).
[[nodiscard]] std::vector<std::string> streamHashArgs(const std::string& input);

/// Per-frame hashes of one stream, used to verify a merged file's decoded video.
[[nodiscard]] std::vector<std::string> frameHashArgs(const std::string& input, int streamIndex);

/// Frame hash of a time range of the source, for comparing a copied region.
[[nodiscard]] std::vector<std::string> rangeStreamHashArgs(const std::string& input, const Rational& start,
                                                          const Rational& end);

/// `-progress pipe:2` lines are key=value pairs; this parses one.
[[nodiscard]] bool parseProgressLine(const std::string& line, std::string* key, std::string* value);

} // namespace monosect::core
