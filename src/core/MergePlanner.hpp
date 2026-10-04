// MonoSect — merge planning.
//
// Merging is the inverse of splitting, and it inherits the same rule: nothing is
// promised that was not checked.  MonoSect concatenates parts with FFmpeg's
// concat demuxer and `-c copy`, which is only valid when every input carries the
// *same* streams in the *same* order with the same codec parameters.  If that is
// not the case the merge is refused with the difference spelled out, instead of
// producing a file whose second half does not decode.
//
// Research notes (docs/RESEARCH.md):
//  * FFmpeg, "concat demuxer" (ffmpeg-formats): the demuxer reads a list of
//    `file` directives; `-safe 0` is required for absolute paths and the list
//    should carry the `ffconcat version 1.0` header; a single quote inside a path
//    has to be escaped as `'\''`.
//  * FFmpeg, "concat" protocol vs demuxer: the demuxer joins at the container
//    level and requires identical stream parameters, which is exactly the
//    condition checked here.
#pragma once

#include "core/MediaModel.hpp"
#include "core/Result.hpp"

#include <string>
#include <vector>

namespace monosect::core {

/// What MonoSect knows about one file that is about to be merged.
struct MergeInput {
    std::string path;
    ByteCount fileSize = 0;
    Rational duration;
    bool durationKnown = false;
    std::string formatNames;
    std::vector<StreamSignature> signatures;
    std::size_t chapterCount = 0;
    std::size_t attachmentCount = 0;
    std::string title;
};

/// Whether a set of inputs can be joined by copying.
struct MergeCompatibility {
    /// True when every input has the same streams with the same parameters.
    bool compatible = false;
    /// True when the files differ only in ways a re-encode could repair.  When
    /// this is false, no merge of these files is possible at all.
    bool reencodeCouldHelp = false;
    /// One sentence per difference, in the user's language.
    std::vector<std::string> differences;
    std::string explanation;
};

struct MergePlan {
    bool planned = false;
    std::string failureExplanation;

    std::vector<MergeInput> inputs;
    std::string outputPath;
    std::string outputExtension;
    std::string containerFormat;      // the value passed to `-f`

    MergeCompatibility compatibility;
    std::vector<StreamSignature> expectedSignatures;
    Rational expectedDuration;
    bool expectedDurationKnown = false;
    ByteCount expectedPayloadBytes = 0;

    std::vector<std::string> warnings;
    std::vector<std::string> notes;

    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string toJson() const;
};

/// Checks whether `candidate` can be concatenated onto `reference`.
///
/// The comparison is on the stream *signature* - codec, profile, level, pixel
/// format, dimensions, sample rate, channel layout, time base - because those are
/// the parameters a demuxer relies on when it joins two decoders into one.  Two
/// files that come from the same split always pass; files from different sources
/// usually do not, and saying so is the point.
[[nodiscard]] MergeCompatibility checkMergeCompatibility(const std::vector<StreamSignature>& reference,
                                                         const std::vector<StreamSignature>& candidate,
                                                         const std::string& referencePath = {},
                                                         const std::string& candidatePath = {});

/// Builds the merge plan for `models` (in the order the user chose).  `outputPath`
/// is the final path the caller has already checked for collisions.
[[nodiscard]] Result<MergePlan> planMerge(const std::vector<MediaModel>& models, const std::string& outputPath);

/// Contents of an FFconcat manifest for `paths`, with the header line and the
/// documented escaping.
[[nodiscard]] std::string concatManifestContents(const std::vector<std::string>& paths);

/// A file name for the manifest that cannot collide with another MonoSect job or
/// with a user file: `.monosect-merge-<token>.ffconcat`.
[[nodiscard]] std::string uniqueManifestName(std::string_view token);

} // namespace monosect::core
