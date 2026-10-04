#include "core/MergePlanner.hpp"

#include "core/Json.hpp"
#include "core/OutputNaming.hpp"
#include "core/ProbeParsing.hpp"
#include "core/Rational.hpp"

#include <cctype>

namespace monosect::core {
namespace {

[[nodiscard]] JsonValue rationalJson(const Rational& value) {
    JsonValue object = JsonValue::makeObject();
    object.set("num", JsonValue::makeInt(value.numerator));
    object.set("den", JsonValue::makeInt(value.denominator));
    object.set("seconds", JsonValue::makeDouble(value.toDouble()));
    return object;
}

[[nodiscard]] std::string describeSignature(const StreamSignature& signature) {
    return "#" + std::to_string(signature.index) + " " + std::string(streamKindName(signature.kind)) + " " +
           signature.describe();
}

/// Fields a re-encode could not repair: the number of streams and their kinds.
[[nodiscard]] bool shapeMatches(const std::vector<StreamSignature>& reference,
                                const std::vector<StreamSignature>& candidate) {
    if (reference.size() != candidate.size()) {
        return false;
    }
    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (reference[i].kind != candidate[i].kind) {
            return false;
        }
    }
    return true;
}

} // namespace

MergeCompatibility checkMergeCompatibility(const std::vector<StreamSignature>& reference,
                                          const std::vector<StreamSignature>& candidate,
                                          const std::string& referencePath,
                                          const std::string& candidatePath) {
    MergeCompatibility result;
    const std::string here = referencePath.empty() ? std::string("the first file") : referencePath;
    const std::string there = candidatePath.empty() ? std::string("this file") : candidatePath;

    if (reference.empty() || candidate.empty()) {
        result.differences.push_back("One of the files reports no streams at all, so there is nothing to join.");
        result.explanation = "A merge needs both files to contain the same streams.";
        return result;
    }
    if (!shapeMatches(reference, candidate)) {
        result.differences.push_back("The files do not carry the same streams: " + here + " has " +
                                     std::to_string(reference.size()) + " and " + there + " has " +
                                     std::to_string(candidate.size()) + ".");
        result.differences.push_back(
            "A merge by copying cannot add, remove or reorder streams; re-encoding cannot repair that either, "
            "because the content would have to be decoded and re-encoded.");
        result.reencodeCouldHelp = false;
        result.explanation = "The files have a different set of streams.";
        return result;
    }

    for (std::size_t i = 0; i < reference.size(); ++i) {
        const StreamSignature& expected = reference[i];
        const StreamSignature& actual = candidate[i];
        const std::string difference = signatureDifference(expected, actual);
        if (!difference.empty()) {
            result.differences.push_back("Stream " + std::to_string(i) + " does not match: " + difference);
        }
    }
    if (reference.front().timebase.isValid() && candidate.front().timebase.isValid() &&
        compareRational(reference.front().timebase, candidate.front().timebase) != 0) {
        result.differences.push_back("The video time base differs (" + rationalToString(reference.front().timebase) +
                                     " against " + rationalToString(candidate.front().timebase) +
                                     "), so frame timestamps would not line up.");
    }

    if (result.differences.empty()) {
        result.compatible = true;
        result.reencodeCouldHelp = false;
        result.explanation = "Every input carries the same streams with the same parameters.";
        return result;
    }

    // Codec/parameter differences can in principle be repaired by re-encoding the
    // differing streams - that changes the media, so it is never the default and
    // it is never done silently.
    result.reencodeCouldHelp = true;
    result.explanation = "The inputs differ in at least one stream parameter, so joining them by copying would "
                         "produce a file whose streams change halfway through.";
    return result;
}

Result<MergePlan> planMerge(const std::vector<MediaModel>& models, const std::string& outputPath) {
    if (models.size() < 2) {
        return failure<MergePlan>(Error{ErrorCode::ConstraintInvalid, "A merge needs at least two files.",
                                        "got " + std::to_string(models.size()),
                                        "Add another file to the merge list."});
    }

    MergePlan plan;
    plan.outputPath = outputPath;
    for (const MediaModel& model : models) {
        MergeInput input;
        input.path = model.path;
        input.fileSize = model.fileSize;
        input.formatNames = model.formatNames;
        input.chapterCount = model.chapters.size();
        input.signatures = signaturesOf(model);
        for (const auto& tag : model.tags) {
            if (tag.first == "title") {
                input.title = tag.second;
            }
        }
        for (const StreamModel& stream : model.streams) {
            if (stream.kind == StreamKind::Attachment) {
                ++input.attachmentCount;
            }
        }
        if (model.durationResolved.known()) {
            input.duration = *model.durationResolved.seconds;
            input.durationKnown = true;
        }
        plan.expectedPayloadBytes = saturatingAdd(plan.expectedPayloadBytes, model.fileSize);
        plan.inputs.push_back(std::move(input));
    }

    // Compatibility: every file against the first one.  Comparing each file to
    // the reference (not to its neighbour) means one bad file is reported once,
    // against the file that defines the stream layout.
    plan.expectedSignatures = plan.inputs.front().signatures;
    bool allCompatible = true;
    bool reencodeCouldHelp = false;
    std::vector<std::string> differences;
    for (std::size_t i = 1; i < plan.inputs.size(); ++i) {
        MergeCompatibility check = checkMergeCompatibility(plan.expectedSignatures, plan.inputs[i].signatures,
                                                           plan.inputs.front().path, plan.inputs[i].path);
        if (!check.compatible) {
            allCompatible = false;
            reencodeCouldHelp = reencodeCouldHelp || check.reencodeCouldHelp;
            for (const std::string& difference : check.differences) {
                differences.push_back(difference);
            }
        }
    }

    plan.compatibility.compatible = allCompatible;
    plan.compatibility.reencodeCouldHelp = reencodeCouldHelp;
    plan.compatibility.differences = differences;
    plan.compatibility.explanation = allCompatible
                                         ? "Every input carries the same streams with the same parameters."
                                         : "The inputs are not interchangeable; see the differences below.";

    // Duration and container.
    bool durationKnown = true;
    Rational total{0, 1};
    for (const MergeInput& input : plan.inputs) {
        if (!input.durationKnown) {
            durationKnown = false;
            continue;
        }
        auto sum = addRational(total, input.duration);
        if (!sum.has_value()) {
            durationKnown = false;
            break;
        }
        total = sum.value();
    }
    plan.expectedDuration = total;
    plan.expectedDurationKnown = durationKnown;

    // The extension is decided here; the matching FFmpeg container name is filled
    // in by the engine (core does not know FFmpeg's own naming).
    plan.outputExtension = extensionForFormat(models.front().formatNames, models.front().path);

    // Container differences are reported, not silently repaired: a merge always
    // writes the container of the *first* input.
    for (std::size_t i = 1; i < plan.inputs.size(); ++i) {
        if (plan.inputs[i].formatNames != plan.inputs.front().formatNames) {
            plan.warnings.push_back("The inputs are not the same container (" + plan.inputs.front().formatNames +
                                    " against " + plan.inputs[i].formatNames +
                                    "); the merged file uses the first file's container, " + plan.outputExtension +
                                    ".");
            break;
        }
    }
    if (plan.inputs.front().chapterCount > 0) {
        plan.notes.push_back("The first input carries " + std::to_string(plan.inputs.front().chapterCount) +
                             " chapter(s) and " + std::to_string(plan.inputs.front().attachmentCount) +
                             " attachment(s); a concatenation keeps the first input's chapters and attachments.");
    }
    for (const MergeInput& input : plan.inputs) {
        if (input.chapterCount > 0 && input.path != plan.inputs.front().path) {
            plan.notes.push_back(input.path + " carries chapters; chapters of inputs other than the first are not "
                                              "carried over by a concatenation.");
            break;
        }
    }

    if (!allCompatible) {
        plan.planned = false;
        plan.failureExplanation =
            "The files cannot be joined by copying. " + plan.compatibility.explanation;
        if (!reencodeCouldHelp) {
            plan.failureExplanation += " Re-encoding cannot repair a different set of streams.";
        } else {
            plan.failureExplanation += " MonoSect does not silently re-encode a merge; the files would have to be "
                                       "converted to a common format first.";
        }
        return plan;
    }

    plan.planned = true;
    return plan;
}

std::string concatManifestContents(const std::vector<std::string>& paths) {
    // FFmpeg's concat demuxer: a version header, then one `file` directive per
    // input.  The path is single-quoted; a quote inside the path is written as
    // '\'' (end the quoted string, add an escaped quote, start a new one), which
    // is the escaping the demuxer documents.
    std::string text = "ffconcat version 1.0\n";
    for (const std::string& path : paths) {
        text += "file '";
        for (char c : path) {
            if (c == '\'') {
                text += "'\\''";
            } else {
                text += c;
            }
        }
        text += "'\n";
    }
    return text;
}

std::string uniqueManifestName(std::string_view token) {
    std::string safe;
    for (char c : token) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
        safe += allowed ? c : '_';
    }
    if (safe.empty()) {
        safe = "job";
    }
    return ".monosect-merge-" + safe + ".ffconcat";
}

std::string MergePlan::describe() const {
    std::string text;
    if (!planned) {
        return "The merge could not be planned: " + failureExplanation;
    }
    text += "Merge plan for " + std::to_string(inputs.size()) + " file(s)\n";
    ByteCount running = 0;
    for (const MergeInput& input : inputs) {
        running = saturatingAdd(running, input.fileSize);
        text += "  " + input.path + ": " + formatBytesShort(input.fileSize) + ", " +
                (input.durationKnown ? formatDuration(input.duration) : std::string("duration unknown")) + ", " +
                friendlyFormatName(input.formatNames) + "\n";
    }
    text += "  output: " + outputPath + " (" + outputExtension + ")\n";
    text += "  streams: " + std::to_string(expectedSignatures.size()) + " (copied unchanged)\n";
    if (expectedDurationKnown) {
        text += "  expected duration: " + formatDuration(expectedDuration) + " (the sum of the parts)\n";
    } else {
        text += "  expected duration: unknown, because at least one input has no trustworthy duration\n";
    }
    text += "  expected size: at least " + formatBytesShort(expectedPayloadBytes) +
            " of media payload, plus the container's own overhead\n";
    for (const std::string& warning : warnings) {
        text += "  warning: " + warning + "\n";
    }
    for (const std::string& note : notes) {
        text += "  note: " + note + "\n";
    }
    return text;
}

std::string MergePlan::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root.set("planned", JsonValue::makeBool(planned));
    if (!planned) {
        root.set("failure", JsonValue::makeString(failureExplanation));
    }
    root.set("output_path", JsonValue::makeString(outputPath));
    root.set("output_extension", JsonValue::makeString(outputExtension));
    root.set("container_format", JsonValue::makeString(containerFormat));
    root.set("compatible", JsonValue::makeBool(compatibility.compatible));
    root.set("reencode_could_help", JsonValue::makeBool(compatibility.reencodeCouldHelp));
    root.set("expected_duration_known", JsonValue::makeBool(expectedDurationKnown));
    root.set("expected_duration", rationalJson(expectedDuration));
    root.set("expected_payload_bytes", JsonValue::makeInt(expectedPayloadBytes));

    JsonValue inputsJson = JsonValue::makeArray();
    for (const MergeInput& input : inputs) {
        JsonValue item = JsonValue::makeObject();
        item.set("path", JsonValue::makeString(input.path));
        item.set("bytes", JsonValue::makeInt(input.fileSize));
        item.set("duration_known", JsonValue::makeBool(input.durationKnown));
        item.set("duration", rationalJson(input.duration));
        item.set("format", JsonValue::makeString(input.formatNames));
        item.set("stream_count", JsonValue::makeInt(static_cast<std::int64_t>(input.signatures.size())));
        inputsJson.push(std::move(item));
    }
    root.set("inputs", std::move(inputsJson));

    JsonValue signaturesJson = JsonValue::makeArray();
    for (const StreamSignature& signature : expectedSignatures) {
        JsonValue item = JsonValue::makeObject();
        item.set("index", JsonValue::makeInt(signature.index));
        item.set("kind", JsonValue::makeString(streamKindName(signature.kind)));
        item.set("codec", JsonValue::makeString(signature.codecName));
        item.set("profile", JsonValue::makeString(signature.profile));
        item.set("level", JsonValue::makeInt(signature.level));
        item.set("width", JsonValue::makeInt(signature.width));
        item.set("height", JsonValue::makeInt(signature.height));
        item.set("pixel_format", JsonValue::makeString(signature.pixelFormat));
        item.set("timebase", JsonValue::makeString(rationalToString(signature.timebase)));
        item.set("description", JsonValue::makeString(describeSignature(signature)));
        signaturesJson.push(std::move(item));
    }
    root.set("expected_signatures", std::move(signaturesJson));

    JsonValue differencesJson = JsonValue::makeArray();
    for (const std::string& difference : compatibility.differences) {
        differencesJson.push(JsonValue::makeString(difference));
    }
    root.set("differences", std::move(differencesJson));

    JsonValue warningsJson = JsonValue::makeArray();
    for (const std::string& warning : warnings) {
        warningsJson.push(JsonValue::makeString(warning));
    }
    root.set("warnings", std::move(warningsJson));
    return root.dump();
}

} // namespace monosect::core
