#include "engine/MergeRunner.hpp"

#include "core/OutputNaming.hpp"
#include "engine/FfmpegArgs.hpp"
#include "engine/JobStore.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <fstream>
#include <sstream>
#include <system_error>

namespace monosect::core {
namespace {

/// Reads one packet's payload bytes straight out of a container file, using the
/// offset ffprobe reported for it.  Used to compare an input's packet with the
/// packet the merged file carries, without trusting either decoder.
[[nodiscard]] bool readPacketBytes(const std::filesystem::path& file, const PacketRecord& packet,
                                   std::string* out) {
    if (packet.filePosition < 0 || packet.sizeBytes <= 0) {
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.seekg(packet.filePosition);
    out->resize(static_cast<std::size_t>(packet.sizeBytes));
    stream.read(out->data(), static_cast<std::streamsize>(out->size()));
    return stream.gcount() == static_cast<std::streamsize>(out->size());
}

/// True when every NAL unit of `part` also appears in `merged`, and the units
/// that are only in `merged` are parameter sets (SPS/PPS/SEI for H.264, the
/// equivalent types for HEVC).
///
/// This is the check behind "the merge added nothing but redundant headers": the
/// concat demuxer writes the next input's parameter sets in band when it switches
/// file, which changes the packet's bytes but not a single decoded sample.
[[nodiscard]] bool packetDiffersOnlyByParameterSets(const std::string& merged, const std::string& part) {
    const auto parse = [](const std::string& data, std::vector<std::string>* units) -> bool {
        std::size_t offset = 0;
        while (offset + 4 <= data.size()) {
            const std::uint32_t length = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
                                         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
                                         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
                                         static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
            offset += 4;
            if (length == 0 || offset + length > data.size()) {
                return false;
            }
            units->emplace_back(data, offset, length);
            offset += length;
        }
        return offset == data.size() && !units->empty();
    };
    std::vector<std::string> mergedUnits;
    std::vector<std::string> partUnits;
    if (!parse(merged, &mergedUnits) || !parse(part, &partUnits)) {
        return false;
    }
    for (const std::string& unit : partUnits) {
        bool found = false;
        for (auto it = mergedUnits.begin(); it != mergedUnits.end(); ++it) {
            if (*it == unit) {
                mergedUnits.erase(it);
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    for (const std::string& unit : mergedUnits) {
        if (unit.empty()) {
            return false;
        }
        const int type = static_cast<unsigned char>(unit[0]) & 0x1F;
        // H.264: 6 = SEI, 7 = SPS, 8 = PPS.  HEVC: 39/40 = SEI, 33 = SPS, 34 = PPS.
        const int hevcType = (static_cast<unsigned char>(unit[0]) >> 1) & 0x3F;
        const bool parameterSet = type == 6 || type == 7 || type == 8 || hevcType == 33 || hevcType == 34 ||
                                  hevcType == 39 || hevcType == 40;
        if (!parameterSet) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string MergeOutcome::summary() const {
    std::string text;
    if (canceled) {
        return "The merge was cancelled." + (path.empty() ? std::string() : " " + path);
    }
    if (!succeeded) {
        return "The merge failed: " + message;
    }
    text = "Merged " + std::to_string(plan.inputs.size()) + " file(s) into " +
           std::filesystem::path(path).filename().string() + " (" + formatBytesExact(fileSizeBytes);
    if (durationMeasured) {
        text += ", " + formatDuration(duration);
    }
    text += "). ";
    text += verified ? "The result was verified: the streams match the inputs' streams and the duration is the sum of "
                       "the parts."
                     : "The result was NOT verified.";
    return text;
}

MergeRunner::MergeRunner(FFprobeService& prober, IProcessRunner& runner, std::string ffmpegPath)
    : prober_(prober), runner_(runner), ffmpegPath_(std::move(ffmpegPath)) {}

Result<MergePlan> MergeRunner::planOnly(const MergeJobRequest& request, const JobEvents& events) {
    const auto output = validateOutputPath(request.outputDirectory, request.outputFileName);
    if (!output.has_value()) {
        return failure<MergePlan>(output.error());
    }
    std::vector<MediaModel> models;
    for (const std::string& input : request.inputs) {
        std::error_code error;
        if (request.overwriteExisting && std::filesystem::equivalent(input, output.value(), error)) {
            return Error{ErrorCode::ConstraintInvalid, "The merged output cannot replace an input file.", input,
                         "Choose a different output name."};
        }
        auto model = prober_.probe(input);
        if (!model.has_value()) {
            return failure<MergePlan>(model.error());
        }
        models.push_back(std::move(model.value()));
        if (events.onLog) {
            events.onLog("probed " + input);
        }
    }
    const std::filesystem::path outputPath =
        std::filesystem::path(request.outputDirectory) / request.outputFileName;
    auto plan = planMerge(models, outputPath.string());
    if (plan.has_value()) {
        plan.value().containerFormat = containerNameForExtension(plan.value().outputExtension);
    }
    return plan;
}

Result<MergeOutcome> MergeRunner::run(const MergeJobRequest& request, const JobEvents& events) {
    // Every terminal state is remembered, exactly as in the split runner: a merge
    // that was refused or that failed verification is a fact about the job
    // history too, and a history that only contains successes cannot be used to
    // tell what was already done.
    auto outcome = runInternal(request, events);
    if (outcome.has_value()) {
        recordJob(request, events, outcome.value());
    } else {
        recordFailure(request, events, outcome.error());
    }
    return outcome;
}

Result<MergeOutcome> MergeRunner::runInternal(const MergeJobRequest& request, const JobEvents& events) {
    const auto started = std::chrono::steady_clock::now();
    MergeOutcome outcome;
    outcome.jobId = makeJobId();
    currentJobId_ = outcome.jobId;
    jobStartedAt_ = currentTimestamp();

    const auto setState = [&events, &outcome](JobState state, const std::string& message) {
        outcome.finalState = state;
        if (events.onState) {
            events.onState(state, message);
        }
    };
    const auto log = [&outcome, &events](const std::string& line) {
        outcome.log.push_back(line);
        if (events.onLog) {
            events.onLog(line);
        }
    };
    const auto canceled = [&events]() { return events.shouldCancel && events.shouldCancel(); };
    const auto frameHashes = [&](const std::filesystem::path& file, int streamIndex,
                                 std::vector<std::string>* out) -> bool {
        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = frameHashArgs(file.string(), streamIndex);
        spec.captureStdout = true;
        spec.captureStderr = true;
        ProcessCallbacks callbacks;
        callbacks.shouldCancel = events.shouldCancel;
        const ProcessResult result = runner_.run(spec, callbacks);
        if (!result.launched || result.exitCode != 0) {
            return false;
        }
        // framemd5 prints "stream, dts, pts, duration, size, hash".  Only the
        // hash identifies the decoded samples: the index and the timestamps of a
        // merged file legitimately differ from the inputs' (the file is one long
        // timeline), the samples must not.
        std::istringstream lines(result.stdoutText);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const std::size_t comma = line.rfind(',');
            out->push_back(comma == std::string::npos ? line : line.substr(comma + 1));
        }
        return true;
    };

    if (request.inputs.size() < 2) {
        return failure<MergeOutcome>(Error{ErrorCode::ConstraintInvalid, "A merge needs at least two files.",
                                           "got " + std::to_string(request.inputs.size()),
                                           "Add another file to the merge list."});
    }
    if (request.outputFileName.empty() || request.outputDirectory.empty()) {
        return failure<MergeOutcome>(Error{ErrorCode::ConstraintInvalid, "A merge needs an output folder and name.",
                                           {}, "Choose where the merged file should be written."});
    }

    setState(JobState::Planning, "Planning the merge");
    auto plan = planOnly(request, events);
    if (!plan.has_value()) {
        outcome.finalState = JobState::Failed;
        outcome.message = plan.error().message;
        setState(JobState::Failed, outcome.message);
        return failure<MergeOutcome>(plan.error());
    }
    outcome.plan = plan.value();
    outcome.plan.containerFormat = containerNameForExtension(outcome.plan.outputExtension);
    if (!outcome.plan.planned) {
        outcome.finalState = JobState::Failed;
        outcome.message = outcome.plan.failureExplanation;
        log(outcome.message);
        for (const std::string& difference : outcome.plan.compatibility.differences) {
            log("  " + difference);
        }
        setState(JobState::Failed, outcome.message);
        return failure<MergeOutcome>(Error{ErrorCode::PlanFailed, outcome.plan.failureExplanation,
                                           describeConstraint(UserConstraint{}),
                                           "Convert the files to one format first, or merge the parts that "
                                           "belong together."});
    }

    const std::filesystem::path outputPath =
        std::filesystem::path(request.outputDirectory) / request.outputFileName;
    outcome.expectedPayloadBytes = outcome.plan.expectedPayloadBytes;

    OutputCommitter committer(std::filesystem::path(request.outputDirectory), request.jobTag,
                              request.overwriteExisting);
    const Status began = committer.begin();
    if (!began.ok()) {
        outcome.finalState = JobState::Failed;
        outcome.message = began.error.message;
        return failure<MergeOutcome>(began.error);
    }

    // The manifest lives in the job folder, so it cannot collide with a user file
    // and two merges running at once cannot overwrite each other's list
    // (TODO 163-165).
    const std::filesystem::path manifestPath = committer.jobDirectory() / uniqueManifestName(outcome.jobId);
    {
        std::ofstream manifest(manifestPath, std::ios::binary | std::ios::trunc);
        if (!manifest) {
            committer.cleanUp();
            return failure<MergeOutcome>(Error{ErrorCode::OutputWriteFailed,
                                               "The merge list could not be written.",
                                               manifestPath.string(),
                                               "Check that the output folder is writable."});
        }
        manifest << concatManifestContents(request.inputs);
        manifest.flush();
        if (!manifest) {
            committer.cleanUp();
            return failure<MergeOutcome>(Error{ErrorCode::OutputWriteFailed,
                                               "The merge list could not be written completely.",
                                               manifestPath.string(), "Check the free space on the output volume."});
        }
    }

    const std::filesystem::path temporaryPath = committer.temporaryPathFor(request.outputFileName);

    setState(JobState::Executing, "Joining " + std::to_string(request.inputs.size()) + " file(s)");
    MergeArgsRequest merge;
    merge.manifest = manifestPath.string();
    merge.output = temporaryPath.string();
    merge.containerFormat = outcome.plan.containerFormat;
    merge.threads = request.threads;

    ProcessSpec spec;
    spec.program = ffmpegPath_;
    spec.arguments = mergeArgs(merge);
    spec.lowPriority = request.lowPriority;
    ProcessCallbacks callbacks;
    callbacks.shouldCancel = events.shouldCancel;
    const std::string commandLine = formatCommandLineForDisplay(spec);
    const ProcessResult result = runner_.run(spec, callbacks);
    if (result.canceled || canceled()) {
        committer.discard(temporaryPath);
        committer.cleanUp();
        outcome.canceled = true;
        outcome.finalState = JobState::Cancelled;
        outcome.message = "Cancelled while joining the files.";
        setState(JobState::Cancelled, outcome.message);
        return outcome;
    }
    if (!result.launched || result.exitCode != 0) {
        committer.discard(temporaryPath);
        if (request.keepTemporaryFilesOnFailure) {
            log("the temporary folder was kept: " + committer.jobDirectory().string());
        } else {
            committer.cleanUp();
        }
        const std::string detail = commandLine + (result.stderrText.empty() ? std::string()
                                                                              : "; " + result.stderrText);
        Error error{ErrorCode::ProcessFailed, "FFmpeg could not join the files.", detail,
                    "Open the log for FFmpeg's own message; the first line usually names the stream that "
                    "differs."};
        outcome.finalState = JobState::Failed;
        outcome.message = error.message;
        setState(JobState::Failed, outcome.message);
        return failure<MergeOutcome>(error);
    }

    // ---- verification ------------------------------------------------
    //
    // A concatenation by copying is exact in a precise sense, and the checks
    // below are the ones that can prove it:
    //
    //  1. the merged file carries the same streams as the inputs (signatures);
    //  2. every input's packets are present: per stream, the merged file has
    //     exactly as many packets as the inputs together;
    //  3. every packet except the first one of each input is *byte identical* to
    //     the input's packet.  The first packet after each junction may be
    //     longer: FFmpeg's demuxer writes the parameter sets (SPS/PPS/SEI) of the
    //     next input in band, which measured as +36 bytes on the H.264 fixture.
    //     That is a redundant copy of information that is already in the
    //     container's header, so it changes no decoded sample - and it is checked
    //     here rather than assumed (only parameter-set NAL units may be added);
    //  4. the merged duration is the sum of the parts, within one packet per
    //     junction (the container-level overlap that a concatenation legitimately
    //     drops);
    //  5. when the caller asked for the deep check, the *decoded video frames*
    //     are the concatenation of the inputs' decoded frames, frame hash by
    //     frame hash.
    setState(JobState::Verifying, "Verifying the merged file");
    VerificationEngine verifier(prober_, runner_, ffmpegPath_, request.verification);
    auto measurement = verifier.measure(temporaryPath, events.shouldCancel);
    if (!measurement.has_value()) {
        committer.discard(temporaryPath);
        committer.cleanUp();
        outcome.finalState = JobState::Failed;
        outcome.message = measurement.error().message;
        setState(JobState::Failed, outcome.message);
        return failure<MergeOutcome>(measurement.error());
    }
    const SegmentMeasurement& measured = measurement.value();
    outcome.fileSizeBytes = measured.fileSizeBytes;
    outcome.durationMeasured = measured.durationKnown;
    outcome.duration = measured.duration;

    bool passed = true;
    if (!measured.exists) {
        passed = false;
        outcome.findings.push_back("The merged file does not exist.");
    }
    if (!measured.readable) {
        passed = false;
        outcome.findings.push_back("The merged file could not be opened by ffprobe.");
    }
    for (const std::string& difference :
         compareSignatureSets(outcome.plan.expectedSignatures, measured.signatures)) {
        passed = false;
        outcome.findings.push_back("The merged file's streams differ from the inputs': " + difference);
    }

    std::vector<PacketIndex> inputPackets;
    if (passed) {
        for (const std::string& input : request.inputs) {
            auto index = prober_.packetIndex(input);
            if (!index.has_value()) {
                passed = false;
                outcome.findings.push_back("The packets of " + input + " could not be read: " +
                                           index.error().message);
                break;
            }
            inputPackets.push_back(std::move(index.value()));
        }
    }

    PacketIndex mergedPackets;
    bool mergedPacketsKnown = false;
    if (passed) {
        auto index = prober_.packetIndex(temporaryPath);
        if (!index.has_value()) {
            passed = false;
            outcome.findings.push_back("The merged file's packets could not be read: " + index.error().message);
        } else {
            mergedPackets = std::move(index.value());
            mergedPacketsKnown = true;
        }
    }

    if (passed) {
        std::map<int, std::int64_t> expectedCount;
        std::map<int, std::int64_t> expectedPayload;
        for (const PacketIndex& index : inputPackets) {
            for (const StreamPacketTrack& track : index.tracks()) {
                expectedCount[track.streamIndex()] += static_cast<std::int64_t>(track.size());
                expectedPayload[track.streamIndex()] += track.totalPayloadBytes();
            }
        }
        for (const StreamPacketTrack& track : mergedPackets.tracks()) {
            const auto count = expectedCount.find(track.streamIndex());
            if (count == expectedCount.end()) {
                passed = false;
                outcome.findings.push_back("The merged file has stream " +
                                           std::to_string(track.streamIndex()) + ", which none of the inputs had.");
                continue;
            }
            if (static_cast<std::int64_t>(track.size()) != count->second) {
                passed = false;
                outcome.findings.push_back("Stream " + std::to_string(track.streamIndex()) + " has " +
                                           std::to_string(track.size()) +
                                           " packet(s) in the merged file but " + std::to_string(count->second) +
                                           " in the inputs together.");
            }
            expectedCount.erase(count);
        }
        for (const auto& missing : expectedCount) {
            passed = false;
            outcome.findings.push_back("Stream " + std::to_string(missing.first) +
                                       " from the inputs is missing in the merged file.");
        }
        if (passed) {
            log("packet counts match: every packet of every input is present in the merged file");
        }
    }

    // Packet-by-packet payload comparison: exact, except that the first packet of
    // each input after the first may carry additional parameter-set NAL units.
    std::int64_t packetsCompared = 0;
    std::int64_t extendedPackets = 0;
    std::int64_t extendedBytes = 0;
    if (passed && mergedPacketsKnown) {
        for (const StreamPacketTrack& mergedTrack : mergedPackets.tracks()) {
            if (!passed) {
                break;
            }
            std::size_t mergedPosition = 0;
            for (std::size_t inputIndex = 0; inputIndex < inputPackets.size() && passed; ++inputIndex) {
                const StreamPacketTrack* track = inputPackets[inputIndex].track(mergedTrack.streamIndex());
                if (track == nullptr) {
                    continue;
                }
                const std::string& inputPath = request.inputs[inputIndex];
                for (std::size_t i = 0; i < track->packets().size() && passed; ++i) {
                    if (mergedPosition >= mergedTrack.packets().size()) {
                        passed = false;
                        outcome.findings.push_back("The merged file ends before the inputs' packets do.");
                        break;
                    }
                    const PacketRecord& expectedPacket = track->packets()[i];
                    const PacketRecord& actualPacket = mergedTrack.packets()[mergedPosition];
                    ++mergedPosition;
                    ++packetsCompared;
                    std::string expectedBytes;
                    std::string actualBytes;
                    if (!readPacketBytes(inputPath, expectedPacket, &expectedBytes) ||
                        !readPacketBytes(temporaryPath, actualPacket, &actualBytes)) {
                        continue;   // no byte offsets in the container: the count check is all that is possible
                    }
                    if (expectedBytes == actualBytes) {
                        continue;
                    }
                    const bool firstOfInput = i == 0;
                    if (!firstOfInput || !packetDiffersOnlyByParameterSets(actualBytes, expectedBytes)) {
                        passed = false;
                        outcome.findings.push_back("A packet of stream " +
                                                   std::to_string(mergedTrack.streamIndex()) +
                                                   " differs from the input's packet (" +
                                                   std::to_string(actualBytes.size()) + " against " +
                                                   std::to_string(expectedBytes.size()) + " bytes).");
                        continue;
                    }
                    ++extendedPackets;
                    extendedBytes += static_cast<std::int64_t>(actualBytes.size()) -
                                     static_cast<std::int64_t>(expectedBytes.size());
                }
            }
            if (mergedPosition != mergedTrack.packets().size() && passed) {
                passed = false;
                outcome.findings.push_back("The merged file has more packets of stream " +
                                           std::to_string(mergedTrack.streamIndex()) + " than the inputs together.");
            }
        }
        if (passed) {
            log("compared " + std::to_string(packetsCompared) + " packet(s) byte for byte");
        }
        if (passed && extendedPackets > 0) {
            log("the first packet of " + std::to_string(extendedPackets) +
                " input(s) carries in-band parameter sets in the merged file (" +
                formatBytesExact(extendedBytes) +
                " extra, redundant: the decoder already has them in the container header)");
        }
    }

    // Duration: the sum of the parts, within one packet per junction.
    if (passed && outcome.plan.expectedDurationKnown && measured.durationKnown && mergedPacketsKnown) {
        Rational tolerance{1, 25};
        for (const StreamPacketTrack& track : mergedPackets.tracks()) {
            const Rational timebase = track.timebase();
            if (!timebase.isValid()) {
                continue;
            }
            for (const PacketRecord& packet : track.packets()) {
                if (!packet.hasDuration) {
                    continue;
                }
                const Rational packetSeconds{packet.durationTicks, timebase.denominator};
                if (packetSeconds > tolerance) {
                    tolerance = packetSeconds;
                }
            }
        }
        auto sum = addRational(tolerance, tolerance);
        if (sum.has_value() && request.inputs.size() > 2) {
            for (std::size_t i = 2; i < request.inputs.size(); ++i) {
                auto grown = addRational(sum.value(), tolerance);
                if (grown.has_value()) {
                    sum = grown;
                }
            }
        }
        const Rational allowedShortfall = sum.has_value() ? sum.value() : tolerance;
        auto shortfall = subtractRational(outcome.plan.expectedDuration, measured.duration);
        if (shortfall.has_value() && shortfall.value() > allowedShortfall) {
            passed = false;
            outcome.findings.push_back("The merged file is shorter than the inputs together: " +
                                       formatDuration(measured.duration) + " against " +
                                       formatDuration(outcome.plan.expectedDuration) + ", which is more than one "
                                       "packet per junction.");
        }
    }

    // Decoded video frames: the merged file must decode to exactly the frames of
    // the inputs, in order.  Audio is not compared this way on purpose: AAC and
    // other lossy audio decoders are stateful, so decoding a part on its own and
    // decoding it after the previous part give slightly different samples near a
    // junction even though the encoded packets are identical.  The packet checks
    // above already prove the encoded audio is present unchanged.
    if (passed && request.verification.decodeCheck && mergedPacketsKnown) {
        for (const StreamPacketTrack& mergedTrack : mergedPackets.tracks()) {
            if (!passed) {
                break;
            }
            const StreamSignature* signature = nullptr;
            for (const StreamSignature& candidate : measured.signatures) {
                if (candidate.index == mergedTrack.streamIndex()) {
                    signature = &candidate;
                }
            }
            if (signature == nullptr || signature->kind != StreamKind::Video) {
                continue;
            }
            std::vector<std::string> mergedFrames;
            if (!frameHashes(temporaryPath, mergedTrack.streamIndex(), &mergedFrames)) {
                passed = false;
                outcome.findings.push_back("The merged video stream could not be decoded for the frame check.");
                break;
            }
            std::vector<std::string> inputFrames;
            for (const std::string& input : request.inputs) {
                std::vector<std::string> frames;
                if (!frameHashes(input, mergedTrack.streamIndex(), &frames)) {
                    passed = false;
                    outcome.findings.push_back("The video stream of " + input + " could not be decoded for the "
                                               "frame check.");
                    break;
                }
                inputFrames.insert(inputFrames.end(), frames.begin(), frames.end());
            }
            if (passed && inputFrames != mergedFrames) {
                passed = false;
                outcome.findings.push_back("The merged video frames are not the inputs' frames in order (" +
                                           std::to_string(mergedFrames.size()) + " against " +
                                           std::to_string(inputFrames.size()) + " frame(s)).");
            } else if (passed) {
                log("decoded video frames match the inputs frame by frame (" +
                    std::to_string(mergedFrames.size()) + " frame(s))");
            }
        }
    }

    if (!passed) {
        for (const std::string& finding : outcome.findings) {
            log("not verified: " + finding);
        }
        outcome.warnings.push_back("The merged file did not pass verification and was not published.");
        if (request.keepTemporaryFilesOnFailure) {
            // The user asked to keep what was written so the failure can be
            // inspected; the file stays in the job folder under its temporary
            // name, so it can never be mistaken for a published result.
            log("the temporary folder was kept: " + committer.jobDirectory().string());
        } else {
            committer.discard(temporaryPath);
            committer.cleanUp();
        }
        outcome.finalState = JobState::Failed;
        outcome.message = "The merged file did not pass verification: " + outcome.findings.front();
        setState(JobState::Failed, outcome.message);
        return failure<MergeOutcome>(Error{ErrorCode::VerificationFailed, outcome.message, commandLine,
                                           "Try merging the parts in the order they were written, or check the log."});
    }
    outcome.verified = true;
    log("verified: " + std::to_string(measured.signatures.size()) + " stream(s), " +
        formatBytesExact(measured.fileSizeBytes) +
        (measured.durationKnown ? ", " + formatDuration(measured.duration) : std::string()));

    setState(JobState::Publishing, "Publishing the merged file");
    const Status flushStatus = committer.flushToDisk(temporaryPath);
    if (!flushStatus.ok()) {
        log(flushStatus.error.toString());
        log("the verified temporary file was kept: " + temporaryPath.string());
        return failure<MergeOutcome>(flushStatus.error);
    }
    const Status commitStatus = committer.commit(temporaryPath, outputPath);
    if (!commitStatus.ok()) {
        committer.discard(temporaryPath);
        committer.cleanUp();
        outcome.finalState = JobState::Failed;
        outcome.message = commitStatus.error.message;
        setState(JobState::Failed, outcome.message);
        return failure<MergeOutcome>(commitStatus.error);
    }
    outcome.path = outputPath.string();
    {
        std::error_code sizeError;
        const std::uintmax_t publishedSize = std::filesystem::file_size(outputPath, sizeError);
        if (!sizeError) {
            outcome.fileSizeBytes = static_cast<ByteCount>(publishedSize);
        }
    }
    committer.cleanUp();

    outcome.succeeded = true;
    outcome.message = "Merged " + std::to_string(request.inputs.size()) + " file(s) into " +
                      outputPath.filename().string();
    outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    setState(JobState::Completed, outcome.message);
    return outcome;
}

void MergeRunner::recordFailure(const MergeJobRequest& request, const JobEvents& events, const Error& error) {
    (void)events;
    if (!request.recordHistory) {
        return;
    }
    JobStore store = request.databasePath.empty() ? JobStore::defaultStore() : JobStore(request.databasePath);
    JobRecord record;
    record.jobId = currentJobId_.empty() ? makeJobId() : currentJobId_;
    record.startedAt = jobStartedAt_;
    record.finishedAt = currentTimestamp();
    record.finalState = jobStateName(JobState::Failed);
    record.message = error.message;
    record.sourcePath = request.inputs.empty() ? std::string() : request.inputs.front();
    record.outputDirectory = request.outputDirectory;
    record.constraintDescription = "merge of " + std::to_string(request.inputs.size()) + " file(s)";
    record.modeDescription = "Merge, Stream copy";
    record.appVersion = request.appVersion;
    record.ffmpegVersion = request.ffmpegVersion;
    record.ffprobeVersion = request.ffprobeVersion;
    record.warnings.push_back(error.toString());
    (void)store.append(record);
}

void MergeRunner::recordJob(const MergeJobRequest& request, const JobEvents& events, MergeOutcome& outcome) {
    if (!request.recordHistory) {
        return;
    }
    JobStore store = request.databasePath.empty() ? JobStore::defaultStore() : JobStore(request.databasePath);
    JobRecord record;
    record.jobId = outcome.jobId;
    record.finishedAt = currentTimestamp();
    record.finalState = jobStateName(outcome.finalState);
    record.message = outcome.message;
    record.sourcePath = request.inputs.empty() ? std::string() : request.inputs.front();
    record.sourceBytes = outcome.expectedPayloadBytes;
    record.sourceDuration = outcome.plan.expectedDuration;
    record.outputDirectory = request.outputDirectory;
    record.constraintDescription =
        "merge of " + std::to_string(request.inputs.size()) + " file(s), copied without re-encoding";
    record.modeDescription = "Merge, Stream copy";
    record.appVersion = request.appVersion;
    record.ffmpegVersion = request.ffmpegVersion;
    record.ffprobeVersion = request.ffprobeVersion;
    record.planJson = outcome.plan.toJson();
    record.warnings = outcome.warnings;

    SegmentOutcome merged;
    merged.index = 1;
    merged.wrote = true;
    merged.path = outcome.path;
    merged.fileSizeBytes = outcome.fileSizeBytes;
    merged.duration = outcome.durationMeasured ? outcome.duration : Rational{0, 1};
    merged.method = SegmentMethod::Copy;
    merged.verified = outcome.verified;
    merged.committed = outcome.succeeded;
    merged.verificationSummary = outcome.verified ? "merged file verified" : "not verified";
    record.parts.push_back(merged);
    record.largestPartBytes = outcome.fileSizeBytes;
    record.longestPartDuration = merged.duration;

    const Status status = store.append(record);
    if (!status.ok()) {
        const std::string warning = "The merge record could not be written to " + store.path().string() + ": " +
                                    status.error.message + " The merged file itself is unaffected.";
        outcome.warnings.push_back(warning);
        outcome.log.push_back(warning);
        if (events.onLog) {
            events.onLog(warning);
        }
    }
}

} // namespace monosect::core
