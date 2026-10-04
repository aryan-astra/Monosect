#include "core/ProbeParsing.hpp"

#include <cctype>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace monosect::core {
namespace {

constexpr std::int64_t kTicksPerSecondFallback = 1000;

std::int64_t parseRationalTicks(const std::string& text, const Rational& timebase, bool* ok) {
    *ok = false;
    if (text.empty() || text == "N/A") {
        return 0;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return 0;
    }
    if (!timebase.isValid() || timebase.numerator == 0) {
        // No timebase: the tick value cannot be turned into seconds, but it is
        // still the value ffprobe reported, so keep it and let the caller decide.
        *ok = true;
        return static_cast<std::int64_t>(value);
    }
    *ok = true;
    return static_cast<std::int64_t>(value);
}

} // namespace

Rational parseFfprobeRational(const std::string& text) {
    if (text.empty() || text == "N/A" || text == "0/0") {
        return Rational{0, 0};
    }
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos) {
        // Some fields (sample_aspect_ratio) are reported as "1:1".
        const std::size_t colon = text.find(':');
        if (colon != std::string::npos) {
            const std::string numerator = text.substr(0, colon);
            const std::string denominator = text.substr(colon + 1);
            return Rational{std::strtoll(numerator.c_str(), nullptr, 10),
                            std::strtoll(denominator.c_str(), nullptr, 10)};
        }
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && end != nullptr && *end == '\0') {
            return Rational{value, 1};
        }
        const double floating = std::strtod(text.c_str(), nullptr);
        return Rational{static_cast<std::int64_t>(floating * 1000000.0), 1000000};
    }
    const std::string numerator = text.substr(0, slash);
    const std::string denominator = text.substr(slash + 1);
    const std::int64_t num = std::strtoll(numerator.c_str(), nullptr, 10);
    const std::int64_t den = std::strtoll(denominator.c_str(), nullptr, 10);
    if (den == 0 || num == 0) {
        return Rational{0, 0};  // "0/0" means unknown for frame rates
    }
    return Rational{num, den};
}

std::optional<std::int64_t> parseFfprobeInteger(const JsonValue& parent, const char* key) {
    const JsonValue* value = parent.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return value->asInt64();
}

std::optional<double> parseFfprobeDouble(const JsonValue& parent, const char* key) {
    const JsonValue* value = parent.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return value->asDouble();
}

std::string parseFfprobeText(const JsonValue& parent, const char* key) {
    const JsonValue* value = parent.find(key);
    if (value == nullptr) {
        return {};
    }
    if (auto text = value->asString(); text.has_value()) {
        if (*text == "N/A") {
            return {};
        }
        return *text;
    }
    if (auto number = value->asInt64(); number.has_value()) {
        return std::to_string(*number);
    }
    if (auto floating = value->asDouble(); floating.has_value()) {
        return std::to_string(*floating);
    }
    return {};
}

std::vector<std::string> probeArguments(const std::string& path, bool wantChapters) {
    // `-show_streams -show_format` returns the complete documented field set; the
    // parser reads it by name.  `-v error` keeps stderr free for real problems,
    // `-of json` is the documented stable machine format.
    std::vector<std::string> arguments{
        "-hide_banner", "-v", "error", "-print_format", "json",
        "-show_format", "-show_streams",
    };
    if (wantChapters) {
        arguments.push_back("-show_chapters");
    }
    arguments.push_back("-i");
    arguments.push_back(path);
    return arguments;
}

std::vector<std::string> packetIndexArguments(const std::string& path, int streamIndex) {
    std::vector<std::string> arguments{
        "-hide_banner", "-v", "error", "-print_format", "json",
        "-show_entries",
        "packet=stream_index,pts,pts_time,dts,dts_time,duration,duration_time,size,pos,flags",
    };
    if (streamIndex >= 0) {
        arguments.push_back("-select_streams");
        arguments.push_back(std::to_string(streamIndex));
    }
    arguments.push_back("-show_packets");
    arguments.push_back("-i");
    arguments.push_back(path);
    return arguments;
}

Result<MediaModel> parseContainerModel(const JsonValue& root, const std::string& path, ByteCount fileSize) {
    const JsonValue* streams = root.find("streams");
    if (streams == nullptr || !streams->isArray()) {
        return Error{ErrorCode::ProbeFailed, "ffprobe did not describe any streams.",
                     "the JSON output has no \"streams\" array",
                     "The file may not be a media file, or ffprobe may have failed."};
    }

    MediaModel model;
    model.path = path;
    model.fileSize = fileSize;

    if (const JsonValue* format = root.find("format"); format != nullptr && format->isObject()) {
        model.formatNames = parseFfprobeText(*format, "format_name");
        model.formatLongName = parseFfprobeText(*format, "format_long_name");
        if (auto duration = parseFfprobeText(*format, "duration"); !duration.empty()) {
            model.duration = parseFfprobeRational(duration);
            if (!model.duration->isValid() || !model.duration->isPositive()) {
                model.duration.reset();
            }
        }
        if (auto size = parseFfprobeInteger(*format, "size"); size.has_value() && *size > 0) {
            model.fileSize = *size;
        } else if (model.fileSize == 0) {
            model.fileSize = 0;
        }
        if (auto bitRate = parseFfprobeInteger(*format, "bit_rate"); bitRate.has_value() && *bitRate > 0) {
            model.bitRate = *bitRate;
        }
        if (auto startTime = parseFfprobeText(*format, "start_time"); !startTime.empty()) {
            model.startTime = parseFfprobeRational(startTime);
        }
        if (const JsonValue* tags = format->find("tags"); tags != nullptr && tags->isObject()) {
            for (const auto& member : tags->members()) {
                if (auto text = member.second.asString(); text.has_value()) {
                    model.tags.emplace_back(member.first, *text);
                }
            }
        }
    }

    model.streams.reserve(streams->size());
    for (const JsonValue& element : streams->items()) {
        if (!element.isObject()) {
            continue;
        }
        StreamModel stream;
        if (auto index = parseFfprobeInteger(element, "index"); index.has_value()) {
            stream.index = static_cast<int>(*index);
        }
        stream.kind = streamKindFromFfprobe(parseFfprobeText(element, "codec_type"));
        stream.codecName = parseFfprobeText(element, "codec_name");
        stream.codecLongName = parseFfprobeText(element, "codec_long_name");
        stream.profile = parseFfprobeText(element, "profile");
        stream.codecTag = parseFfprobeText(element, "codec_tag_string");
        if (auto level = parseFfprobeInteger(element, "level"); level.has_value()) {
            stream.level = static_cast<int>(*level);
        }
        if (auto width = parseFfprobeInteger(element, "width"); width.has_value()) {
            stream.width = static_cast<int>(*width);
        }
        if (auto height = parseFfprobeInteger(element, "height"); height.has_value()) {
            stream.height = static_cast<int>(*height);
        }
        stream.sampleAspectRatio = parseFfprobeRational(parseFfprobeText(element, "sample_aspect_ratio"));
        stream.displayAspectRatio = parseFfprobeRational(parseFfprobeText(element, "display_aspect_ratio"));
        stream.averageFrameRate = parseFfprobeRational(parseFfprobeText(element, "avg_frame_rate"));
        stream.baseFrameRate = parseFfprobeRational(parseFfprobeText(element, "r_frame_rate"));
        stream.pixelFormat = parseFfprobeText(element, "pix_fmt");
        if (auto hasB = parseFfprobeInteger(element, "has_b_frames"); hasB.has_value()) {
            stream.hasBFrames = static_cast<int>(*hasB);
        }
        if (auto refs = parseFfprobeInteger(element, "refs"); refs.has_value()) {
            stream.referenceFrames = static_cast<int>(*refs);
        }
        if (auto sampleRate = parseFfprobeInteger(element, "sample_rate"); sampleRate.has_value() && *sampleRate > 0) {
            stream.sampleRate = static_cast<int>(*sampleRate);
        }
        if (auto channels = parseFfprobeInteger(element, "channels"); channels.has_value() && *channels > 0) {
            stream.channels = static_cast<int>(*channels);
        }
        stream.channelLayout = parseFfprobeText(element, "channel_layout");
        if (auto bits = parseFfprobeInteger(element, "bits_per_sample"); bits.has_value() && *bits > 0) {
            stream.bitsPerSample = static_cast<int>(*bits);
        }

        const std::string timebaseText = parseFfprobeText(element, "time_base");
        stream.timebase = parseFfprobeRational(timebaseText);
        stream.timebaseKnown = stream.timebase.isValid() && stream.timebase.numerator > 0 &&
                               stream.timebase.denominator > 0;

        bool ok = false;
        if (const std::string startPts = parseFfprobeText(element, "start_pts"); !startPts.empty()) {
            const std::int64_t value = parseRationalTicks(startPts, stream.timebase, &ok);
            if (ok) {
                stream.startPts = value;
            }
        }
        if (const std::string startTime = parseFfprobeText(element, "start_time"); !startTime.empty()) {
            const Rational parsed = parseFfprobeRational(startTime);
            if (parsed.isValid()) {
                stream.startTime = parsed;
            }
        }
        if (const std::string durationTicks = parseFfprobeText(element, "duration_ts"); !durationTicks.empty()) {
            const std::int64_t value = parseRationalTicks(durationTicks, stream.timebase, &ok);
            if (ok && value > 0) {
                stream.durationTicks = value;
            }
        }
        if (const std::string duration = parseFfprobeText(element, "duration"); !duration.empty()) {
            const Rational parsed = parseFfprobeRational(duration);
            if (parsed.isValid() && parsed.isPositive()) {
                stream.duration = parsed;
            }
        }
        if (auto bitRate = parseFfprobeInteger(element, "bit_rate"); bitRate.has_value() && *bitRate > 0) {
            stream.bitRate = *bitRate;
        }
        if (auto nbFrames = parseFfprobeInteger(element, "nb_frames"); nbFrames.has_value() && *nbFrames > 0) {
            stream.frameCount = *nbFrames;
        }

        // Display matrix: ffprobe reports it as side data on the stream.  The
        // "rotate" tag is the old way of writing the same thing and current
        // FFmpeg ignores it, so the side data is the only reliable source.
        if (const JsonValue* sideData = element.find("side_data_list");
            sideData != nullptr && sideData->isArray()) {
            for (const JsonValue& entry : sideData->items()) {
                if (!entry.isObject()) {
                    continue;
                }
                const std::string type = parseFfprobeText(entry, "side_data_type");
                if (type.find("Display Matrix") == std::string::npos) {
                    continue;
                }
                std::optional<std::int64_t> degrees;
                if (auto value = parseFfprobeInteger(entry, "rotation"); value.has_value()) {
                    degrees = *value;
                } else {
                    // Newer ffprobe prints a float; it is still a whole number of
                    // degrees in practice.
                    const std::string text = parseFfprobeText(entry, "rotation");
                    if (!text.empty()) {
                        const double parsed = std::strtod(text.c_str(), nullptr);
                        degrees = static_cast<std::int64_t>(parsed);
                    }
                }
                if (!degrees.has_value()) {
                    continue;
                }
                std::int64_t normalized = *degrees % 360;
                if (normalized < 0) {
                    normalized += 360;
                }
                // Round to the nearest quarter turn: a matrix that is not a
                // multiple of 90 degrees cannot be reproduced by any of the
                // supported modes, and claiming otherwise would be a lie.
                const std::int64_t quarter = ((normalized + 45) / 90) % 4;
                stream.rotationDegrees = static_cast<int>(quarter * 90);
                stream.rotationKnown = true;
                break;
            }
        }

        if (const JsonValue* disposition = element.find("disposition"); disposition != nullptr && disposition->isObject()) {
            if (auto flag = parseFfprobeInteger(*disposition, "default"); flag.has_value()) {
                stream.isDefault = *flag != 0;
            }
            if (auto flag = parseFfprobeInteger(*disposition, "forced"); flag.has_value()) {
                stream.isForced = *flag != 0;
            }
            if (auto flag = parseFfprobeInteger(*disposition, "attached_pic"); flag.has_value()) {
                stream.attachedPicture = static_cast<int>(*flag);
            }
        }

        if (const JsonValue* tags = element.find("tags"); tags != nullptr && tags->isObject()) {
            for (const auto& member : tags->members()) {
                if (auto text = member.second.asString(); text.has_value()) {
                    stream.tags.emplace_back(member.first, *text);
                    if (member.first == "language") {
                        stream.language = *text;
                    } else if (member.first == "title") {
                        stream.title = *text;
                    } else if (member.first == "handler_name") {
                        stream.handlerName = *text;
                    }
                }
            }
        }

        model.streams.push_back(std::move(stream));
    }

    if (model.streams.empty()) {
        return Error{ErrorCode::UnsupportedInputFormat, "This file has no readable streams.",
                     "path: " + path,
                     "Check that the file is a media file and that it is not damaged."};
    }

    // Chapter parsing
    if (const JsonValue* chapters = root.find("chapters"); chapters != nullptr && chapters->isArray()) {
        for (const JsonValue& element : chapters->items()) {
            if (!element.isObject()) {
                continue;
            }
            ChapterModel chapter;
            if (auto id = parseFfprobeInteger(element, "id"); id.has_value()) {
                chapter.id = static_cast<int>(*id);
            }
            chapter.timebase = parseFfprobeRational(parseFfprobeText(element, "time_base"));
            if (auto start = parseFfprobeInteger(element, "start"); start.has_value()) {
                chapter.startTicks = *start;
            }
            if (auto end = parseFfprobeInteger(element, "end"); end.has_value()) {
                chapter.endTicks = *end;
            }
            if (const JsonValue* tags = element.find("tags"); tags != nullptr && tags->isObject()) {
                chapter.title = parseFfprobeText(*tags, "title");
            }
            if (chapter.timebase.isValid()) {
                model.chapters.push_back(std::move(chapter));
            }
        }
    }

    // Fill in the derived facts that validation and planning rely on.
    model.durationResolved = model.durationTruth();
    model.durationMissing = !model.durationResolved.known();
    if (const StreamModel* video = model.videoStream(); video != nullptr) {
        model.frameRate.averageFrameRate = video->averageFrameRate;
        model.frameRate.baseFrameRate = video->baseFrameRate;
        const bool haveBoth = video->averageFrameRate.isValid() && video->averageFrameRate.numerator > 0 &&
                              video->baseFrameRate.isValid() && video->baseFrameRate.numerator > 0;
        if (haveBoth) {
            // A single frame rate is reported when the ratio is within one frame
            // per hour of each other; anything larger is treated as variable.
            const Rational difference{1, 3600};
            const int comparison = compareRational(video->averageFrameRate, video->baseFrameRate);
            const Rational& larger = comparison > 0 ? video->averageFrameRate : video->baseFrameRate;
            const Rational& smaller = comparison > 0 ? video->baseFrameRate : video->averageFrameRate;
            Rational gap{0, 1};
            if (smaller.isPositive()) {
                // gap = larger - smaller, using rational arithmetic (the values
                // are frame rates with small denominators, so this is exact).
                if (auto difference = subtractRational(larger, smaller); difference.has_value()) {
                    gap = difference.value();
                }
            }
            model.frameRate.variable = gap > difference;
            model.frameRate.explanation =
                model.frameRate.variable
                    ? "The video stream is variable frame rate (avg " + rationalToString(video->averageFrameRate) +
                          " fps, base " + rationalToString(video->baseFrameRate) +
                          " fps), so a frame count is not a duration."
                    : "The video stream reports a single frame rate (" + rationalToString(video->baseFrameRate) +
                          " fps).";
        } else {
            model.frameRate.explanation = "The video stream does not report a usable frame rate.";
        }
    }

    // A still image (a single frame) cannot be split by time.
    if (const StreamModel* video = model.videoStream();
        video != nullptr && model.streams.size() == 1 && video->frameCount.has_value() && *video->frameCount == 1) {
        model.isStillImage = true;
    }
    if (!model.duration.has_value() && model.startTime.isValid() && model.startTime.toDouble() < -0.001) {
        // Negative start times with no container duration usually indicate a live
        // or growing file; the planner treats "unknown duration" explicitly.
        model.isLiveStreamLike = true;
    }

    return model;
}

Result<PacketIndex> parsePacketSection(const JsonValue& root, const MediaModel& model,
                                       std::size_t* packetsWithoutTimestamp) {
    const JsonValue* packets = root.find("packets");
    if (packets == nullptr) {
        return Error{ErrorCode::ProbeFailed, "ffprobe did not return a packet list.",
                     "the JSON output has no \"packets\" member",
                     "This is an internal error; please report it."};
    }
    return buildPacketIndex(*packets, model, packetsWithoutTimestamp);
}

bool isTransportStreamFormat(std::string_view formatNames) {
    for (const std::string_view name : {"mpegts", "mpegtsraw", "mpeg", "dvb", "sat", "ip"}) {
        if (formatNames.find(name) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool isMatroskaFormat(std::string_view formatNames) {
    return formatNames.find("matroska") != std::string_view::npos ||
           formatNames.find("webm") != std::string_view::npos;
}

bool isIsoBaseMediaFormat(std::string_view formatNames) {
    for (const std::string_view name : {"mov", "mp4", "m4a", "3gp", "3g2", "mj2", "ismv", "heic"}) {
        if (formatNames.find(name) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

std::string friendlyFormatName(std::string_view formatNames) {
    if (isMatroskaFormat(formatNames)) {
        // ffprobe names the Matroska demuxer "matroska,webm" for every Matroska
        // file, WebM included, so the *format list* cannot tell them apart.  The
        // list names matroska first and WebM is a restricted profile of it, so
        // without a path the honest name is Matroska.  friendlyFormatName(model)
        // adds the extension and then says "MKV" or "WebM".
        return formatNames.find("matroska") != std::string_view::npos ? "Matroska" : "WebM";
    }
    if (isIsoBaseMediaFormat(formatNames)) {
        // Never "M4A" here: the format name list of every MP4 contains "m4a".
        return "MP4/MOV";
    }
    if (isTransportStreamFormat(formatNames)) {
        return "MPEG-TS";
    }
    if (formatNames.find("avi") != std::string_view::npos) {
        return "AVI";
    }
    if (formatNames.find("flv") != std::string_view::npos) {
        return "FLV";
    }
    if (formatNames.find("ogg") != std::string_view::npos) {
        return "Ogg";
    }
    if (formatNames.find("wav") != std::string_view::npos) {
        return "WAV";
    }
    return std::string(formatNames);
}

std::string friendlyFormatName(const ContainerModel& model) {
    const std::string& names = model.formatNames;
    if (isIsoBaseMediaFormat(names)) {
        const bool hasVideo = model.firstStreamOfKind(StreamKind::Video) != nullptr;
        const bool hasAudio = model.firstStreamOfKind(StreamKind::Audio) != nullptr;
        std::string extension;
        if (const std::size_t dot = model.path.find_last_of('.'); dot != std::string::npos) {
            extension = model.path.substr(dot + 1);
            for (char& c : extension) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        if (!hasVideo && hasAudio && (extension == "m4a" || extension == "m4b" || extension == "mp4a")) {
            return "M4A";
        }
        if (extension == "mov" && !hasVideo) {
            return "MOV";
        }
        return "MP4/MOV";
    }
    if (isMatroskaFormat(names)) {
        // The demuxer's name list is the same for both, so the extension is the
        // only thing that distinguishes a .mkv from a .webm here.
        std::string extension = model.path;
        if (const std::size_t dot = extension.find_last_of('.'); dot != std::string::npos) {
            extension = extension.substr(dot + 1);
        } else {
            extension.clear();
        }
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return extension == "webm" ? "WebM" : "MKV";
    }
    return friendlyFormatName(names);
}

std::vector<std::string> requiredBitstreamFilters(std::string_view codecName, std::string_view sourceFormatNames,
                                                  std::string_view targetFormatNames) {
    std::vector<std::string> filters;
    const bool targetIsTransportStream = isTransportStreamFormat(targetFormatNames);
    const bool sourceIsIsoBaseMedia = isIsoBaseMediaFormat(sourceFormatNames);
    if (!targetIsTransportStream || !sourceIsIsoBaseMedia) {
        return filters;
    }
    // MP4/MOV store AVCC length-prefixed NAL units; transport streams need the
    // Annex-B start-code form.
    if (codecName == "h264") {
        filters.emplace_back("h264_mp4toannexb");
    } else if (codecName == "hevc") {
        filters.emplace_back("hevc_mp4toannexb");
    } else if (codecName == "mpeg4" || codecName == "mpeg2video") {
        filters.emplace_back("mpeg4_unpack_bframes");
    } else if (codecName == "aac") {
        filters.emplace_back("aac_adtstoasc");
    }
    return filters;
}

} // namespace monosect::core
