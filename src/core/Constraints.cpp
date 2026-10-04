#include "core/Constraints.hpp"

#include "core/ProbeParsing.hpp"

#include <algorithm>
#include <set>

namespace monosect::core {
namespace {

enum class ContainerFamily {
    IsoBaseMedia,
    Matroska,
    WebM,
    TransportStream,
    Avi,
    Flv,
    Ogg,
    Wave,
    MpegAudio,
    Unknown,
};

[[nodiscard]] std::string lowercaseExtension(std::string_view path) {
    std::string extension;
    if (const std::size_t dot = path.find_last_of('.'); dot != std::string_view::npos) {
        extension = std::string(path.substr(dot + 1));
    }
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

ContainerFamily familyOf(std::string_view formatNames, std::string_view sourcePath = {}) {
    if (isMatroskaFormat(formatNames)) {
        // ffprobe reports "matroska,webm" for every Matroska file, WebM included:
        // "webm" there is a demuxer name, not a statement about the file.  WebM is
        // a restricted profile of Matroska - it cannot hold H.264, HEVC or AAC -
        // so treating an .mkv as WebM would refuse perfectly normal splits.  The
        // extension is the one reliable signal.
        if (lowercaseExtension(sourcePath) == "webm") {
            return ContainerFamily::WebM;
        }
        if (formatNames.find("matroska") == std::string_view::npos) {
            return ContainerFamily::WebM;
        }
        return ContainerFamily::Matroska;
    }
    if (isIsoBaseMediaFormat(formatNames)) {
        return ContainerFamily::IsoBaseMedia;
    }
    if (isTransportStreamFormat(formatNames)) {
        return ContainerFamily::TransportStream;
    }
    if (formatNames.find("avi") != std::string_view::npos) {
        return ContainerFamily::Avi;
    }
    if (formatNames.find("flv") != std::string_view::npos) {
        return ContainerFamily::Flv;
    }
    if (formatNames.find("ogg") != std::string_view::npos) {
        return ContainerFamily::Ogg;
    }
    if (formatNames.find("wav") != std::string_view::npos) {
        return ContainerFamily::Wave;
    }
    if (formatNames.find("mp3") != std::string_view::npos || formatNames.find("mp2") != std::string_view::npos) {
        return ContainerFamily::MpegAudio;
    }
    return ContainerFamily::Unknown;
}

bool listContains(const std::initializer_list<const char*>& list, std::string_view value) {
    for (const char* candidate : list) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

/// Codecs that the container is designed to hold.  The lists are intentionally
/// short: only codecs that FFmpeg can actually mux there without extra handling
/// are listed, and everything else is reported as "would have to be re-encoded"
/// rather than being attempted.
bool familyAccepts(ContainerFamily family, StreamKind kind, std::string_view codecName) {
    switch (family) {
        case ContainerFamily::IsoBaseMedia:
            if (kind == StreamKind::Video) {
                return listContains({"h264", "hevc", "mpeg4", "mpeg2video", "av1", "vp9", "mjpeg", "prores",
                                     "dnxhd", "rawvideo"},
                                    codecName);
            }
            if (kind == StreamKind::Audio) {
                return listContains({"aac", "mp3", "ac3", "eac3", "alac", "opus", "flac", "mp2", "pcm_s16le",
                                     "pcm_s24le", "twos"},
                                    codecName);
            }
            if (kind == StreamKind::Subtitle) {
                return listContains({"mov_text", "ttml", "webvtt", "eia_608"}, codecName);
            }
            return false;

        case ContainerFamily::Matroska:
            // Matroska is the one container that can hold practically everything,
            // which is why it is the recommended target for "keep every stream".
            return kind != StreamKind::Unknown;

        case ContainerFamily::WebM:
            if (kind == StreamKind::Video) {
                return listContains({"vp8", "vp9", "av1"}, codecName);
            }
            if (kind == StreamKind::Audio) {
                return listContains({"opus", "vorbis"}, codecName);
            }
            if (kind == StreamKind::Subtitle) {
                return listContains({"webvtt"}, codecName);
            }
            return false;

        case ContainerFamily::TransportStream:
            if (kind == StreamKind::Video) {
                return listContains({"h264", "hevc", "mpeg2video", "mpeg1video", "av1", "vc1"}, codecName);
            }
            if (kind == StreamKind::Audio) {
                return listContains({"aac", "ac3", "eac3", "mp2", "mp3", "opus"}, codecName);
            }
            // DVB subtitles and teletext are possible but need to be signalled
            // explicitly; MonoSect reports them as unsupported instead of
            // producing a file where the player silently loses them.
            return false;

        case ContainerFamily::Avi:
            if (kind == StreamKind::Video) {
                return listContains({"mpeg4", "mjpeg", "h264", "mpeg2video", "rawvideo", "ffv1"}, codecName);
            }
            if (kind == StreamKind::Audio) {
                return listContains({"mp3", "ac3", "pcm_s16le", "pcm_s24le", "mp2"}, codecName);
            }
            return false;

        case ContainerFamily::Flv:
            if (kind == StreamKind::Video) {
                return listContains({"h264", "flv1", "vp6a", "vp6f"}, codecName);
            }
            if (kind == StreamKind::Audio) {
                return listContains({"aac", "mp3", "nellymoser", "speex"}, codecName);
            }
            return false;

        case ContainerFamily::Ogg:
            if (kind == StreamKind::Video) {
                return listContains({"theora", "vp8"}, codecName);
            }
            if (kind == StreamKind::Audio) {
                return listContains({"vorbis", "opus", "flac", "speex"}, codecName);
            }
            if (kind == StreamKind::Subtitle) {
                return listContains({"webvtt", "kate"}, codecName);
            }
            return false;

        case ContainerFamily::Wave:
            return kind == StreamKind::Audio &&
                   (codecName.rfind("pcm_", 0) == 0 || listContains({"adpcm_ms", "alac"}, codecName));

        case ContainerFamily::MpegAudio:
            return kind == StreamKind::Audio && listContains({"mp3", "mp2"}, codecName);

        case ContainerFamily::Unknown:
            // An unknown container: do not claim anything either way.
            return true;
    }
    return true;
}

std::string streamLabel(const StreamModel& stream) {
    return std::string(streamKindName(stream.kind)) + " stream " + std::to_string(stream.index) + " (" +
           stream.codecName + ")";
}

} // namespace

const char* preservationModeName(PreservationMode mode) noexcept {
    switch (mode) {
        case PreservationMode::StreamCopy: return "Stream copy";
        case PreservationMode::KeyframeAligned: return "Keyframe aligned copy";
        case PreservationMode::SmartCut: return "Smart cut";
        case PreservationMode::FullReEncode: return "Full re-encode";
    }
    return "Unknown";
}

const char* preservationModeGuarantee(PreservationMode mode) noexcept {
    switch (mode) {
        case PreservationMode::StreamCopy:
            return "Every packet is copied bit for bit; parts begin at an existing keyframe of the source.";
        case PreservationMode::KeyframeAligned:
            return "Every packet is copied bit for bit; part boundaries are the keyframes closest to the "
                   "requested positions.";
        case PreservationMode::SmartCut:
            return "The body of each part is copied bit for bit; only the region at a boundary is re-encoded so "
                   "the boundary can be placed exactly where you asked.";
        case PreservationMode::FullReEncode:
            return "Nothing is copied; every frame is re-encoded. Use this only when the container or codec must "
                   "change.";
    }
    return "";
}

std::string preservationModeDetail(PreservationMode mode) {
    switch (mode) {
        case PreservationMode::StreamCopy:
            return "Quality loss: none. The cut points are limited to keyframes that already exist in the source. "
                   "On a file with sparse keyframes a part can be noticeably larger or smaller than requested.";
        case PreservationMode::KeyframeAligned:
            return "Quality loss: none. MonoSect chooses the keyframe that gets closest to the requested position "
                   "and reports the difference for every part.";
        case PreservationMode::SmartCut:
            return "Quality loss: only inside a region of roughly one to three seconds at each boundary, which is "
                   "re-encoded at a high quality setting. The result is not bit-identical to the source and "
                   "MonoSect never calls it lossless.";
        case PreservationMode::FullReEncode:
            return "Quality loss: the whole file. This is the only mode that can guarantee a byte budget on a "
                   "source whose codec does not allow small enough keyframe steps.";
    }
    return "";
}

const char* precisionPolicyName(PrecisionPolicy policy) noexcept {
    switch (policy) {
        case PrecisionPolicy::Fast: return "Fast";
        case PrecisionPolicy::Balanced: return "Balanced";
        case PrecisionPolicy::Maximum: return "Maximum precision";
    }
    return "Unknown";
}

std::string precisionPolicyDetail(PrecisionPolicy policy) {
    switch (policy) {
        case PrecisionPolicy::Fast:
            return "Predicts part sizes from the source index and writes each part once. Fastest, still never "
                   "exceeds a hard limit.";
        case PrecisionPolicy::Balanced:
            return "Predicts from the source index, then measures the first written part and corrects the "
                   "prediction for the rest. One extra pass at most per part to get closer to the limit.";
        case PrecisionPolicy::Maximum:
            return "Adds a short calibration render before the first part and searches between candidate "
                   "boundaries so that every part is as large as it can safely be. Uses more CPU and disk time.";
    }
    return "";
}

bool containerAcceptsCodec(std::string_view formatNames, std::string_view codecName, StreamKind kind,
                           std::string_view sourcePath) {
    return familyAccepts(familyOf(formatNames, sourcePath), kind, codecName);
}

std::string codecCompatibilityNote(std::string_view formatNames, std::string_view codecName, StreamKind kind,
                                   std::string_view sourcePath) {
    ContainerModel probe;
    probe.formatNames = std::string(formatNames);
    probe.path = std::string(sourcePath);
    const std::string container = friendlyFormatName(probe);
    if (kind == StreamKind::Video) {
        if (container == "WebM") {
            return "WebM can only hold VP8, VP9 or AV1 video, so " + std::string(codecName) +
                   " would have to be re-encoded to one of them (or choose MKV, which keeps it as it is).";
        }
        if (container == "MP4/MOV") {
            return "MP4 does not accept " + std::string(codecName) +
                   " without re-encoding; MKV accepts it unchanged.";
        }
        return container + " does not accept the video codec " + std::string(codecName) + " unchanged.";
    }
    if (kind == StreamKind::Audio) {
        return container + " does not accept the audio codec " + std::string(codecName) +
               " unchanged (MKV and MP4 accept most codecs).";
    }
    if (kind == StreamKind::Subtitle) {
        return container + " does not accept the subtitle format " + std::string(codecName) +
               " unchanged; MKV accepts most subtitle formats, other containers usually do not.";
    }
    if (kind == StreamKind::Attachment) {
        return container + " cannot carry file attachments; MKV can.";
    }
    return container + " does not accept the stream type of " + std::string(codecName) + ".";
}

std::vector<const StreamModel*> requiredStreams(const UserConstraint& constraint, const MediaModel& model) {
    std::vector<const StreamModel*> out;
    if (constraint.requiredStreamIndexes.empty()) {
        for (const auto& stream : model.streams) {
            out.push_back(&stream);
        }
        return out;
    }
    for (const int index : constraint.requiredStreamIndexes) {
        if (const StreamModel* stream = model.stream(index); stream != nullptr) {
            out.push_back(stream);
        }
    }
    return out;
}

std::string describeConstraint(const UserConstraint& constraint) {
    std::string text;
    if (constraint.hasSizeLimit()) {
        text += "at most " + formatBytesWithExact(constraint.size->maxBytes);
        if (constraint.size->minBytes.has_value()) {
            text += " (aiming for at least " + formatBytesShort(*constraint.size->minBytes) + ")";
        }
    }
    if (constraint.hasDurationLimit()) {
        if (!text.empty()) {
            text += ", ";
        }
        text += "at most " + formatDuration(constraint.duration->maxSeconds) + " per part";
    }
    if (text.empty()) {
        text = "no hard limit";
    }
    text += "; " + std::string(preservationModeName(constraint.preservation));
    text += ", " + std::string(precisionPolicyName(constraint.precision)) + " precision";
    if (constraint.maxParts.has_value()) {
        text += ", at most " + std::to_string(*constraint.maxParts) + " parts";
    }
    return text;
}

ConstraintReport validateConstraint(const UserConstraint& constraint, const MediaModel& model) {
    ConstraintReport report;

    const auto error = [&report](std::string message, std::string remedy) {
        report.valid = false;
        report.issues.push_back(ConstraintIssue{ConstraintIssue::Severity::Error, std::move(message), std::move(remedy)});
    };
    const auto warning = [&report](std::string message, std::string remedy) {
        report.issues.push_back(
            ConstraintIssue{ConstraintIssue::Severity::Warning, std::move(message), std::move(remedy)});
    };

    if (!constraint.hasAnyHardLimit() && !constraint.splitAtSeconds.has_value() &&
        !constraint.targetPartSeconds.has_value()) {
        error("No limit and no split position were given.",
              "Set a maximum size or duration, or a position to cut at.");
    }

    if (constraint.targetPartSeconds.has_value() && !constraint.targetPartSeconds->isPositive()) {
        error("The target part duration is zero.",
              "Enter a positive duration such as 90 s, or clear the field.");
    }

    if (model.isStillImage) {
        error("This file contains a single image and cannot be split into time ranges.",
              "Use an image converter instead; MonoSect splits media over time.");
    }

    if (model.streams.empty()) {
        error("This file has no streams MonoSect can work with.", "Choose a different file.");
    }

    if (model.durationMissing) {
        if (constraint.hasDurationLimit()) {
            error("A duration limit needs a known duration, and this file does not report one.",
                  "Use a size limit instead, or remux the file so that it carries a duration.");
        } else {
            warning("This file does not report a duration.",
                    "Size-based splitting still works because packet sizes are known, but MonoSect cannot state "
                    "how long the last part will be.");
        }
    } else if (!model.durationResolved.reliable && constraint.hasDurationLimit()) {
        warning("The duration is an estimate (" + model.durationResolved.explanation + ").",
                "MonoSect verifies the real duration of every part after writing it and will shorten a part that "
                "turns out to be too long.");
    }

    if (constraint.hasSizeLimit()) {
        // 64 KiB: below this, container headers alone exceed the limit for every
        // format MonoSect writes, so the request is refused instead of producing
        // a file that cannot hold its own index.
        constexpr ByteCount kMinimumSensible = 64 * kKiB;
        if (constraint.size->maxBytes < kMinimumSensible) {
            error("A size limit of " + formatBytesShort(constraint.size->maxBytes) +
                      " is too small to hold a media file.",
                  "Use a limit of at least 64 KiB (" + formatBytesShort(kMinimumSensible) + ").");
        }
        if (constraint.size->minBytes.has_value() && *constraint.size->minBytes > constraint.size->maxBytes) {
            error("The lower bound is larger than the hard maximum.",
                  "Lower the \"aim for at least\" value or raise the hard maximum.");
        }
        if (model.fileSize > 0 && constraint.size->maxBytes >= model.fileSize) {
            warning("The size limit is not smaller than the source, so a single part is enough.",
                    "MonoSect will still verify the result in full.");
        }
        if (model.fileSize > 0 && constraint.size->maxBytes > 0) {
            const ByteCount estimatedParts = (model.fileSize / constraint.size->maxBytes) + 1;
            if (estimatedParts > 500) {
                warning("This limit will produce roughly " + std::to_string(estimatedParts) + " parts.",
                        "Very small parts mean a slow job and many container headers; raise the limit if you can.");
            }
            if (estimatedParts > 20000) {
                error("This limit would produce more than 20 000 parts.",
                      "Raise the limit; MonoSect refuses to create a job this fragmented.");
            }
        }
    }

    if (constraint.hasDurationLimit()) {
        const Rational& limit = constraint.duration->maxSeconds;
        if (compareRational(limit, Rational{1, 2}) < 0) {
            warning("A duration limit below half a second leaves very little room for container overhead.",
                    "Consider a larger limit, or a size limit instead.");
        }
        if (model.durationResolved.known() && compareRational(limit, *model.durationResolved.seconds) >= 0) {
            warning("The duration limit is not smaller than the source, so a single part is enough.",
                    "MonoSect will still verify the result in full.");
        }
    }

    if (constraint.splitAtSeconds.has_value()) {
        if (compareRational(*constraint.splitAtSeconds, Rational{0, 1}) <= 0) {
            error("The split position must be after the start of the file.",
                  "Enter a time such as 00:30:00.");
        } else if (model.durationResolved.known() &&
                   compareRational(*constraint.splitAtSeconds, *model.durationResolved.seconds) >= 0) {
            error("The split position is at or past the end of the file.",
                  "Enter a time that is inside the file.");
        }
        if (constraint.hasAnyHardLimit()) {
            warning("A split position and a hard limit were both given.",
                    "MonoSect will cut at the requested position and then split the remainder by the limit.");
        }
    }

    // Stream preservation: every required stream has to be able to survive the
    // chosen mode and target container.
    const std::vector<const StreamModel*> streams = requiredStreams(constraint, model);
    if (streams.empty()) {
        error("No streams were selected for preservation.",
              "Select at least one stream, or leave the selection empty to keep all of them.");
    }
    for (const StreamModel* stream : streams) {
        const bool copyMode = constraint.preservation != PreservationMode::FullReEncode;
        if (!copyMode) {
            if (stream->kind == StreamKind::Attachment || stream->kind == StreamKind::Data) {
                warning(streamLabel(*stream) + " cannot be re-encoded.",
                        "Re-encoding drops this stream; choose a copy mode or an MKV target to keep it.");
            } else if (stream->kind == StreamKind::Subtitle) {
                warning(streamLabel(*stream) + " would be converted to text; subtitle conversion is lossy.",
                        "Choose a copy mode to keep the subtitle stream as it is.");
            }
            continue;
        }
        if (!containerAcceptsCodec(model.formatNames, stream->codecName, stream->kind, model.path)) {
            if (stream->kind == StreamKind::Video || stream->kind == StreamKind::Audio) {
                error(streamLabel(*stream) + " cannot be copied into " +
                          friendlyFormatName(model) + ".",
                      codecCompatibilityNote(model.formatNames, stream->codecName, stream->kind, model.path) +
                          " MonoSect will not silently re-encode it; switch to Smart cut or Full re-encode, or "
                          "keep the source container.");
            } else {
                error(streamLabel(*stream) + " cannot be copied at all.",
                      codecCompatibilityNote(model.formatNames, stream->codecName, stream->kind, model.path));
            }
        }
    }

    if (constraint.preservation == PreservationMode::SmartCut) {
        const StreamModel* video = model.videoStream();
        if (video == nullptr) {
            warning("Smart cut needs a video stream; this file has none.",
                    "MonoSect will fall back to keyframe-aligned copying and say so in the plan.");
        } else if (video->codecName != "h264" && video->codecName != "hevc" && video->codecName != "vp9" &&
                   video->codecName != "vp8" && video->codecName != "av1" && video->codecName != "mpeg4" &&
                   video->codecName != "mpeg2video") {
            warning("Smart cut has no encoder mapping for " + video->codecName + " video.",
                    "MonoSect will use keyframe-aligned copying instead; the boundary will be a keyframe, not "
                    "the exact requested time.");
        }
    }

    if (constraint.preservation == PreservationMode::StreamCopy && constraint.splitAtSeconds.has_value()) {
        warning("An exact split position cannot be guaranteed while copying without re-encoding.",
                "MonoSect will cut at the closest keyframe and report how far it was from the requested time. "
                "Choose Smart cut to cut exactly there.");
    }

    return report;
}

} // namespace monosect::core
