#include "engine/FfmpegArgs.hpp"

#include "core/ProbeParsing.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace monosect::core {
namespace {

std::string numberText(std::int64_t value) {
    return std::to_string(value);
}

} // namespace

std::string formatSecondsForFfmpeg(const Rational& seconds) {
    if (!seconds.isValid()) {
        return "0";
    }
    const long double value = seconds.toDouble();
    const bool negative = value < 0;
    const long double magnitude = std::fabs(value);
    // Round to the nearest microsecond; FFmpeg's time base is exactly 1/1000000
    // for the `-ss`/`-t`/`-to` options, so nothing finer can be expressed anyway.
    const long long microseconds = static_cast<long long>(std::llround(magnitude * 1000000.0L));
    const long long wholeSeconds = microseconds / 1000000;
    const long long fraction = microseconds % 1000000;
    char buffer[64];
    if (fraction == 0) {
        std::snprintf(buffer, sizeof(buffer), "%s%lld", negative ? "-" : "", wholeSeconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s%lld.%06lld", negative ? "-" : "", wholeSeconds, fraction);
    }
    return buffer;
}

std::string containerNameForExtension(const std::string& extension) {
    if (extension == "mkv") {
        return "matroska";
    }
    if (extension == "webm") {
        return "webm";
    }
    if (extension == "mp4") {
        return "mp4";
    }
    if (extension == "mov") {
        return "mov";
    }
    if (extension == "m4a") {
        return "ipod";
    }
    if (extension == "ts") {
        return "mpegts";
    }
    if (extension == "avi") {
        return "avi";
    }
    if (extension == "flv") {
        return "flv";
    }
    if (extension == "ogg") {
        return "ogg";
    }
    if (extension == "wav") {
        return "wav";
    }
    if (extension == "mp3") {
        return "mp3";
    }
    return extension;
}

std::string encoderForCodec(std::string_view codecName, StreamKind kind) {
    if (kind == StreamKind::Video) {
        if (codecName == "h264") return "libx264";
        if (codecName == "hevc") return "libx265";
        if (codecName == "vp9") return "libvpx-vp9";
        if (codecName == "vp8") return "libvpx";
        if (codecName == "av1") return "libsvtav1";
        if (codecName == "mpeg4") return "mpeg4";
        if (codecName == "mpeg2video") return "mpeg2video";
        if (codecName == "mpeg1video") return "mpeg1video";
        if (codecName == "mjpeg") return "mjpeg";
        if (codecName == "ffv1") return "ffv1";
        if (codecName == "rawvideo") return "rawvideo";
        return {};
    }
    if (kind == StreamKind::Audio) {
        if (codecName == "aac") return "aac";
        if (codecName == "opus") return "libopus";
        if (codecName == "vorbis") return "libvorbis";
        if (codecName == "mp3") return "libmp3lame";
        if (codecName == "ac3") return "ac3";
        if (codecName == "eac3") return "eac3";
        if (codecName == "flac") return "flac";
        if (codecName == "alac") return "alac";
        if (codecName == "pcm_s16le") return "pcm_s16le";
        if (codecName == "pcm_s24le") return "pcm_s24le";
        if (codecName == "pcm_s32le") return "pcm_s32le";
        return {};
    }
    return {};
}

std::vector<std::string> qualityArgumentsFor(std::string_view codecName, int crf) {
    std::vector<std::string> arguments;
    if (codecName == "h264") {
        arguments = {"-crf", numberText(crf), "-preset", "medium"};
    } else if (codecName == "hevc") {
        arguments = {"-crf", numberText(crf), "-preset", "medium", "-tag:v", "hvc1"};
    } else if (codecName == "vp9") {
        arguments = {"-crf", numberText(crf + 13), "-b:v", "0"};
    } else if (codecName == "vp8") {
        arguments = {"-crf", numberText(crf + 12), "-b:v", "0"};
    } else if (codecName == "av1") {
        arguments = {"-crf", numberText(crf + 12), "-preset", "6"};
    } else if (codecName == "mpeg4" || codecName == "mpeg2video" || codecName == "mpeg1video") {
        arguments = {"-q:v", "2"};
    } else if (codecName == "ffv1") {
        arguments = {"-level", "3"};
    }
    return arguments;
}

std::vector<std::string> renderSegmentArgs(const SegmentRenderRequest& request) {
    std::vector<std::string> arguments;
    arguments.push_back("-hide_banner");
    arguments.push_back("-nostdin");
    arguments.push_back("-loglevel");
    arguments.push_back("error");
    arguments.push_back("-nostats");
    if (request.writeProgress) {
        arguments.push_back("-progress");
        arguments.push_back("pipe:2");
    }
    if (request.threads > 0) {
        arguments.push_back("-threads");
        arguments.push_back(numberText(request.threads));
    }

    // Input seeking.  With `-c copy` FFmpeg keeps the packets from the keyframe
    // before the requested position, so a copy boundary has to be a keyframe -
    // the planner guarantees that.  When re-encoding, the same seek is accurate
    // because FFmpeg decodes and discards the extra frames by default.
    arguments.push_back("-ss");
    arguments.push_back(formatSecondsForFfmpeg(request.start));
    arguments.push_back("-i");
    arguments.push_back(request.input);

    Rational duration = subtractRational(request.end, request.start).value_or(Rational{0, 1});
    if (!duration.isPositive()) {
        duration = Rational{1, 1000};
    }
    arguments.push_back("-t");
    arguments.push_back(formatSecondsForFfmpeg(duration));

    // Stream selection: exactly the streams the plan names, independently of the
    // order FFmpeg would choose for -map 0.
    if (request.copiedStreams.empty() && request.reencodedStreams.empty()) {
        arguments.push_back("-map");
        arguments.push_back("0");
    } else {
        for (const int index : request.copiedStreams) {
            arguments.push_back("-map");
            arguments.push_back("0:" + numberText(index));
        }
        for (const auto& stream : request.reencodedStreams) {
            arguments.push_back("-map");
            arguments.push_back("0:" + numberText(stream.index));
        }
    }

    arguments.push_back("-c");
    arguments.push_back("copy");

    // Counting how often a kind is re-encoded decides whether the quality options
    // can be written globally or have to be attached to one stream.  With more
    // than one stream of a kind the per-stream form is used, which FFmpeg
    // documents as `-option:stream_index`.
    std::size_t reencodedVideoCount = 0;
    std::size_t reencodedAudioCount = 0;
    for (const auto& stream : request.reencodedStreams) {
        if (stream.kind == StreamKind::Video) {
            ++reencodedVideoCount;
        } else if (stream.kind == StreamKind::Audio) {
            ++reencodedAudioCount;
        }
    }

    for (const auto& stream : request.reencodedStreams) {
        const std::string encoder = encoderForCodec(stream.codecName, stream.kind);
        if (encoder.empty()) {
            // The caller validated this before starting; be explicit anyway.
            continue;
        }
        const std::string suffix = ":" + numberText(stream.index);
        arguments.push_back("-c" + suffix);
        arguments.push_back(encoder);

        const std::vector<std::string> quality = qualityArgumentsFor(stream.codecName, request.crf);
        if (stream.kind == StreamKind::Video) {
            if (reencodedVideoCount == 1) {
                for (const std::string& option : quality) {
                    arguments.push_back(option);
                }
            } else {
                // Re-attach each option to this stream: "-crf" becomes "-crf:3".
                for (std::size_t i = 0; i + 1 < quality.size(); i += 2) {
                    arguments.push_back(quality[i] + suffix);
                    arguments.push_back(quality[i + 1]);
                }
            }
            if (!request.pixelFormat.empty()) {
                arguments.push_back("-pix_fmt" + std::string(reencodedVideoCount == 1 ? "" : suffix));
                arguments.push_back(request.pixelFormat);
            }
            if (!request.profile.empty()) {
                arguments.push_back("-profile:v" + std::string(reencodedVideoCount == 1 ? "" : suffix));
                arguments.push_back(request.profile);
            }
            if (request.frameRateNumerator > 0 && request.frameRateDenominator > 0) {
                arguments.push_back("-r" + std::string(reencodedVideoCount == 1 ? "" : suffix));
                arguments.push_back(numberText(request.frameRateNumerator) + "/" +
                                    numberText(request.frameRateDenominator));
            }
            if (stream.bitRate > 0) {
                // Cap the encoder near the source's own bit rate so the result
                // cannot silently become much larger than the original.
                arguments.push_back("-maxrate" + suffix);
                arguments.push_back(numberText(stream.bitRate));
                arguments.push_back("-bufsize" + suffix);
                arguments.push_back(numberText(stream.bitRate * 2));
            }
        } else if (stream.kind == StreamKind::Audio) {
            if (stream.bitRate > 0) {
                arguments.push_back("-b:a" + suffix);
                arguments.push_back(numberText(stream.bitRate));
            }
        }
    }

    // Exact end control for the passes that write several streams at once
    // (re-encode and smart cut).  A count may only be used for one stream per
    // FFmpeg run, so the caller passes at most one trim here; the copy path
    // trims each stream in its own run instead.
    for (const StreamTrim& trim : request.trims) {
        if (!trim.hasPacketCount) {
            continue;
        }
        arguments.push_back("-frames:" + numberText(static_cast<std::int64_t>(trim.outputStreamIndex)));
        arguments.push_back(numberText(trim.packetCount));
    }

    if (request.copyMetadata) {
        arguments.push_back("-map_metadata");
        arguments.push_back("0");
    } else {
        arguments.push_back("-map_metadata");
        arguments.push_back("-1");
    }
    // Source chapters are not copied into every part: they would be duplicated in
    // each one.  Chapter handling is a separate feature and is reported in the plan.
    arguments.push_back("-map_chapters");
    arguments.push_back("-1");
    if (request.avoidNegativeTimestamps) {
        arguments.push_back("-avoid_negative_ts");
        arguments.push_back("make_zero");
    }
    if (!request.containerFormat.empty()) {
        arguments.push_back("-f");
        arguments.push_back(request.containerFormat);
    }
    arguments.push_back("-y");
    arguments.push_back(request.output);
    return arguments;
}

std::vector<std::string> concatArgs(const std::string& listPath, const std::string& output,
                                    const std::string& containerFormat) {
    std::vector<std::string> arguments{"-hide_banner", "-nostdin", "-loglevel", "error", "-nostats",
                                       "-progress", "pipe:2"};
    arguments.push_back("-f");
    arguments.push_back("concat");
    arguments.push_back("-safe");
    arguments.push_back("0");
    arguments.push_back("-i");
    arguments.push_back(listPath);
    arguments.push_back("-c");
    arguments.push_back("copy");
    arguments.push_back("-map_metadata");
    arguments.push_back("0");
    arguments.push_back("-map_chapters");
    arguments.push_back("-1");
    arguments.push_back("-avoid_negative_ts");
    arguments.push_back("make_zero");
    if (!containerFormat.empty()) {
        arguments.push_back("-f");
        arguments.push_back(containerFormat);
    }
    arguments.push_back("-y");
    arguments.push_back(output);
    return arguments;
}

Status writeConcatList(const std::filesystem::path& listPath,
                       const std::vector<std::filesystem::path>& parts) {
    std::ofstream out(listPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return makeError(ErrorCode::OutputWriteFailed, "The temporary file list could not be written.",
                         "path: " + listPath.string(), "Check that the output folder is writable.");
    }
    for (const auto& part : parts) {
        // Relative siblings keep Win32 extended-path prefixes out of FFmpeg's concat parser.
        std::string path = part.parent_path() == listPath.parent_path() ? part.filename().generic_string()
                                                                       : part.generic_string();
        std::string escaped;
        for (const char c : path) {
            if (c == '\'') {
                escaped += "'\\''";
            } else {
                escaped.push_back(c);
            }
        }
        out << "file '" << escaped << "'\n";
    }
    out.flush();
    if (!out) {
        return makeError(ErrorCode::OutputWriteFailed, "The temporary file list could not be written.",
                         "path: " + listPath.string(), "Check that the output folder is writable.");
    }
    return Status::success();
}

std::vector<std::string> copyWindowArgs(const CopyWindowRequest& request) {
    std::vector<std::string> arguments{"-hide_banner", "-nostdin", "-loglevel", "error", "-nostats"};
    if (request.writeProgress) {
        arguments.push_back("-progress");
        arguments.push_back("pipe:2");
    }
    if (request.threads > 0) {
        arguments.push_back("-threads");
        arguments.push_back(numberText(request.threads));
    }
    // Fast input seek: it only has to *contain* what the trims need, so landing
    // early is harmless and saves a full read of the source.
    arguments.push_back("-ss");
    arguments.push_back(formatSecondsForFfmpeg(request.requestedStart));
    arguments.push_back("-i");
    arguments.push_back(request.input);
    for (const int index : request.streams) {
        arguments.push_back("-map");
        arguments.push_back("0:" + numberText(index));
    }
    arguments.push_back("-c");
    arguments.push_back("copy");
    if (request.length.isPositive()) {
        arguments.push_back("-t");
        arguments.push_back(formatSecondsForFfmpeg(request.length));
    }
    arguments.push_back("-map_metadata");
    arguments.push_back("0");
    arguments.push_back("-map_chapters");
    arguments.push_back("-1");
    arguments.push_back("-f");
    arguments.push_back(request.containerFormat);
    arguments.push_back("-y");
    arguments.push_back(request.output);
    return arguments;
}

std::vector<std::string> trimStreamArgs(const StreamTrimRequest& request) {
    std::vector<std::string> arguments{"-hide_banner", "-nostdin", "-loglevel", "error", "-nostats"};
    // -avoid_negative_ts is a muxer option, so it belongs with the other output
    // options below rather than before -i.
    //
    // It is here for a concrete failure.  A stream-copy pass that starts at a
    // seek position produces, in addition to the packets, one packet carrying the
    // "skip this much pre-roll" hint - and that packet carries *no timestamp at
    // all*.  The muxer of this first pass accepts it, so the trimmed intermediate
    // looks fine and the problem is invisible.  The second pass then re-reads that
    // intermediate and muxes it into the real output, where Matroska refuses a
    // packet with an unknown timestamp:
    //
    //   [matroska] Can't write packet with unknown timestamp
    //   Error muxing a packet ... Task finished with error code: -22 (Invalid argument)
    //
    // and the job stops on its last part - the part that has no successor to fall
    // back on.  FFmpeg says so itself when the option is absent: "failed to avoid
    // negative pts -40 in stream 0. Try -avoid_negative_ts 1 as a possible
    // workaround."
    //
    // Measured on this machine with a real HEVC fixture, the part boundary at
    // 0:04.000 and 50 intended packets:
    //
    //   without   50 packets, 1 with no timestamp, re-mux fails with -22
    //   with      50 packets, 0 with no timestamp, re-mux succeeds
    //
    // make_zero is the right variant rather than make_non_negative or 1 because the
    // trimmed intermediate is an internal artefact that is re-muxed into a part
    // starting at zero anyway: shifting its first packet to zero is exactly what
    // the next pass needs, and no information is lost because the packet index
    // MonoSect verified is derived from the plan, not from the intermediate's
    // timestamps.
    //
    // It is appended *after* the seek arguments on purpose.  FFmpeg classifies an
    // option by where it appears, so an option placed before -i is an input option
    // and is rejected here: measured, `-avoid_negative_ts` before -i fails with
    // "you are trying to apply an input option to an output file or vice versa".
    if (request.seekMode == StreamTrimRequest::SeekMode::InputKeyframe) {
        // Input seek: FFmpeg starts at the keyframe at or before the position.
        // Exact because the planner only puts copy boundaries on keyframes.
        arguments.push_back("-ss");
        arguments.push_back(formatSecondsForFfmpeg(request.start));
        arguments.push_back("-i");
        arguments.push_back(request.input);
    } else {
        // Output seek: FFmpeg discards the packets before the position and starts
        // at the first packet at or after it.  This reads the file from its start,
        // which is the price of an exact start for streams that have no keyframes.
        arguments.push_back("-i");
        arguments.push_back(request.input);
        arguments.push_back("-ss");
        arguments.push_back(formatSecondsForFfmpeg(request.start));
    }
    arguments.push_back("-map");
    arguments.push_back("0:" + numberText(request.inputStreamIndex));
    arguments.push_back("-c");
    arguments.push_back("copy");
    if (request.hasPacketCount) {
        // The output of this pass has exactly one stream, so its output index is
        // 0 and the count cannot be confused with another stream's.
        arguments.push_back("-frames:0");
        arguments.push_back(numberText(request.packetCount));
    }
    arguments.push_back("-map_metadata");
    arguments.push_back("0");
    arguments.push_back("-map_chapters");
    arguments.push_back("-1");
    arguments.push_back("-avoid_negative_ts");
    arguments.push_back("make_zero");
    arguments.push_back("-f");
    arguments.push_back(request.containerFormat);
    arguments.push_back("-y");
    arguments.push_back(request.output);
    return arguments;
}

std::vector<std::string> mergeArgs(const MergeArgsRequest& request) {
    std::vector<std::string> arguments{"-hide_banner", "-nostdin", "-loglevel", "error", "-nostats"};
    if (request.writeProgress) {
        arguments.push_back("-progress");
        arguments.push_back("pipe:2");
    }
    if (request.threads > 0) {
        arguments.push_back("-threads");
        arguments.push_back(numberText(request.threads));
    }
    arguments.push_back("-f");
    arguments.push_back("concat");
    arguments.push_back("-safe");
    arguments.push_back("0");
    arguments.push_back("-i");
    arguments.push_back(request.manifest);
    arguments.push_back("-map");
    arguments.push_back("0");
    arguments.push_back("-c");
    arguments.push_back("copy");
    arguments.push_back("-map_metadata");
    arguments.push_back("0");
    arguments.push_back("-map_chapters");
    arguments.push_back("0");
    arguments.push_back("-f");
    arguments.push_back(request.containerFormat);
    arguments.push_back("-y");
    arguments.push_back(request.output);
    return arguments;
}

std::vector<std::string> muxStreamsArgs(const MuxStreamsRequest& request) {
    std::vector<std::string> arguments{"-hide_banner", "-nostdin", "-loglevel", "error", "-nostats"};
    if (request.writeProgress) {
        arguments.push_back("-progress");
        arguments.push_back("pipe:2");
    }
    if (request.threads > 0) {
        arguments.push_back("-threads");
        arguments.push_back(numberText(request.threads));
    }
    if (request.preserveTimestamps) {
        // A global option: it must come before the first input to apply to all of
        // them.  See MuxStreamsRequest::preserveTimestamps for the measurement.
        arguments.push_back("-copyts");
    }
    for (const std::string& input : request.inputs) {
        arguments.push_back("-i");
        arguments.push_back(input);
    }
    const bool haveMetadataSource = !request.metadataSource.empty();
    if (haveMetadataSource) {
        // The original file is opened for its metadata, and additionally as the
        // source of any packetless stream: no timed media of it is read, because
        // an attachment or data stream has no timeline to read a range of.
        arguments.push_back("-i");
        arguments.push_back(request.metadataSource);
    }
    for (std::size_t index = 0; index < request.inputs.size(); ++index) {
        arguments.push_back("-map");
        arguments.push_back(numberText(static_cast<std::int64_t>(index)));
    }
    // The packetless streams are addressed as `<source input>:<stream>`, so the
    // mapping has to name the stream rather than the whole input the way the timed
    // ones above do.  See MuxStreamsRequest::packetlessStreams for why they cannot
    // be carried by a trimmed intermediate.
    for (const int streamIndex : request.packetlessStreams) {
        arguments.push_back("-map");
        arguments.push_back(numberText(static_cast<std::int64_t>(request.inputs.size())) + ":" +
                            numberText(streamIndex));
    }
    if (haveMetadataSource) {
        arguments.push_back("-map_metadata");
        arguments.push_back(numberText(static_cast<std::int64_t>(request.inputs.size())));
    } else {
        arguments.push_back("-map_metadata");
        arguments.push_back("-1");
    }
    arguments.push_back("-map_chapters");
    arguments.push_back("-1");
    arguments.push_back("-c");
    arguments.push_back("copy");
    // The per-stream intermediates are joined with -copyts above so that a stream
    // whose first packet is not exactly on the origin - audio, almost always -
    // lands in the right place.  That preservation is what carries the
    // "skip this much pre-roll" packet into this pass, and Matroska refuses to
    // write a packet with no timestamp at all:
    //
    //   [matroska] Can't write packet with unknown timestamp
    //   Task finished with error code: -22 (Invalid argument)
    //
    // so the job fails on the part it is writing.  -avoid_negative_ts make_zero
    // resolves that packet's timestamp against the output's own zero instead of
    // propagating the unknown one.  Measured on this machine, the last part of a
    // two-part HEVC split at a 0:04.000 boundary: it fails with -22 without this
    // option and is written and verified with it.
    arguments.push_back("-avoid_negative_ts");
    arguments.push_back("make_zero");
    arguments.push_back("-f");
    arguments.push_back(request.containerFormat);
    arguments.push_back("-y");
    arguments.push_back(request.output);
    return arguments;
}

std::vector<std::string> decodeCheckArgs(const std::string& input) {
    return {"-hide_banner", "-nostdin", "-loglevel", "error", "-xerror", "-i", input, "-f", "null", "-"};
}

std::vector<std::string> streamHashArgs(const std::string& input) {
    return {"-hide_banner", "-nostdin", "-loglevel", "error", "-i", input,
            "-map", "0", "-c", "copy", "-f", "streamhash", "-hash", "sha256", "-"};
}

std::vector<std::string> frameHashArgs(const std::string& input, int streamIndex) {
    // One line per decoded frame (video) or per decoded packet (audio), with the
    // hash of the decoded samples.  Used to prove a merged file decodes to
    // exactly the frames of the files it was built from.
    return {"-hide_banner", "-nostdin", "-loglevel", "error", "-i", input,
            "-map", "0:" + numberText(streamIndex), "-f", "framemd5", "-hash", "sha256", "-"};
}

std::vector<std::string> rangeStreamHashArgs(const std::string& input, const Rational& start, const Rational& end) {
    Rational duration = subtractRational(end, start).value_or(Rational{0, 1});
    return {"-hide_banner", "-nostdin", "-loglevel", "error", "-ss", formatSecondsForFfmpeg(start), "-i", input,
            "-t", formatSecondsForFfmpeg(duration), "-map", "0", "-c", "copy", "-f", "streamhash", "-hash",
            "sha256", "-"};
}

bool parseProgressLine(const std::string& line, std::string* key, std::string* value) {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos || separator == 0) {
        return false;
    }
    if (key != nullptr) {
        *key = line.substr(0, separator);
    }
    if (value != nullptr) {
        *value = line.substr(separator + 1);
    }
    return true;
}

} // namespace monosect::core
