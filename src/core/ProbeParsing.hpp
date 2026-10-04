// MonoSect — parsing of ffprobe output.
//
// The parser is written against the *documented* ffprobe JSON field names, never
// against position in the array.  Every field that is documented as a string in
// ffprobe output ("N/A", "30000/1001", "0/0") is handled explicitly: a value that
// cannot be interpreted becomes "unknown" and is reported, never guessed.
//
// Research notes (docs/RESEARCH.md):
//  * ffprobe(1) documents `-show_format`, `-show_streams`, `-show_chapters`,
//    `-show_packets` and `-show_entries`; values such as duration, bit_rate and
//    nb_frames are reported as strings and may be "N/A".
//  * `-of json` is documented to be stable, and `packets` are reported in decode
//    order with `size`, `pos`, `pts`, `dts`, `duration` and `flags` per packet.
#pragma once

#include "core/Json.hpp"
#include "core/MediaModel.hpp"
#include "core/PacketIndex.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace monosect::core {

/// Result of one ffprobe run, already parsed.
struct ProbeSnapshot {
    MediaModel container;
    PacketIndex packets;
    bool hasPacketIndex = false;
    std::size_t packetsWithoutTimestamp = 0;
    std::size_t packetsRejected = 0;

    [[nodiscard]] bool usable() const noexcept { return !container.streams.empty(); }
};

/// Builds the argument list for the container probe.  No shell is involved: the
/// list is passed to CreateProcessW/execv as it is.
[[nodiscard]] std::vector<std::string> probeArguments(const std::string& path, bool wantChapters);
/// Builds the argument list for the packet index run.
[[nodiscard]] std::vector<std::string> packetIndexArguments(const std::string& path, int streamIndex = -1);

/// Parses `{ "streams": [...], "format": {...}, "chapters": [...] }`.
[[nodiscard]] Result<MediaModel> parseContainerModel(const JsonValue& root, const std::string& path,
                                                    ByteCount fileSize);

/// Parses `{ "packets": [...] }` into an index for the given model.
[[nodiscard]] Result<PacketIndex> parsePacketSection(const JsonValue& root, const MediaModel& model,
                                                     std::size_t* packetsWithoutTimestamp = nullptr);

/// "30000/1001" -> 30000/1001; "0/0" and "N/A" -> an invalid rational (unknown).
[[nodiscard]] Rational parseFfprobeRational(const std::string& text);

/// Reads a numeric field that ffprobe may print as a number or as a string.
[[nodiscard]] std::optional<std::int64_t> parseFfprobeInteger(const JsonValue& parent, const char* key);
[[nodiscard]] std::optional<double> parseFfprobeDouble(const JsonValue& parent, const char* key);
[[nodiscard]] std::string parseFfprobeText(const JsonValue& parent, const char* key);

/// Bitstream filters that have to be inserted when the named codec is copied
/// between the two container families.  Copying without them produces a file
/// that some players refuse; MonoSect reports this instead of producing it
/// silently.
[[nodiscard]] std::vector<std::string> requiredBitstreamFilters(std::string_view codecName,
                                                                std::string_view sourceFormatNames,
                                                                std::string_view targetFormatNames);

/// True when the format name list identifies an MPEG transport stream family.
[[nodiscard]] bool isTransportStreamFormat(std::string_view formatNames);
/// True when the format name list identifies a Matroska/WebM family.
[[nodiscard]] bool isMatroskaFormat(std::string_view formatNames);
/// True when the format name list identifies an ISO base media (MP4/MOV) family.
[[nodiscard]] bool isIsoBaseMediaFormat(std::string_view formatNames);
/// A short, human-facing container name: "MP4", "MKV", "WebM", "TS", or the raw
/// list.
///
/// The format name list of an ISO base media file is always
/// "mov,mp4,m4a,3gp,3g2,mj2" - ffprobe lists every demuxer that can open it, so
/// no file can be identified as M4A (or, for that matter, as MKV rather than
/// WebM: both report "matroska,webm") from the list alone.  This overload is
/// therefore deliberately vague - "MP4/MOV", "Matroska" - and the model
/// overload, which also sees the file extension, is what the UI uses.
[[nodiscard]] std::string friendlyFormatName(std::string_view formatNames);

/// The same name, decided from the streams and the file extension as well: an
/// audio-only ISO base media file whose name ends in .m4a/.m4b/.mp4a is called
/// M4A, everything else in that family is MP4/MOV, and a Matroska file is called
/// MKV or WebM according to its extension.  This is the overload the UI, the logs
/// and validation use.
[[nodiscard]] std::string friendlyFormatName(const ContainerModel& model);

} // namespace monosect::core
