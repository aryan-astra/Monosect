#include "core/SplitPlanner.hpp"

#include "core/ProbeParsing.hpp"

#include <filesystem>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace monosect::core {
namespace {

constexpr ByteCount kUnlimited = std::numeric_limits<ByteCount>::max();

std::string secondsText(const Rational& seconds) {
    return formatDuration(seconds);
}

} // namespace

SplitPlanner::SplitPlanner(const MediaModel& model, const PacketIndex& index, const UserConstraint& constraint,
                           const NamingPolicy& naming, SizeModel sizeModel, PlannerSettings settings)
    : model_(model), index_(index), constraint_(constraint), naming_(naming), sizeModel_(std::move(sizeModel)),
      settings_(settings) {
    const StreamPacketTrack* track = boundaryTrack();
    if (track == nullptr) {
        stats_.gridDescription = "no stream carries usable timestamps";
    } else if (model_.videoStream() != nullptr && track->streamIndex() == model_.videoStream()->index) {
        stats_.gridDescription = "boundaries follow the keyframes of video stream " +
                                 std::to_string(track->streamIndex()) + " (" +
                                 std::to_string(track->keyframeCount()) + " keyframes found)";
    } else {
        stats_.gridDescription = "this file has no video stream, so boundaries follow the packet grid of stream " +
                                 std::to_string(track->streamIndex());
    }
}

Rational SplitPlanner::gridTimebase() const {
    if (const StreamModel* video = model_.videoStream(); video != nullptr && video->timebaseKnown) {
        return video->timebase;
    }
    if (const StreamPacketTrack* track = boundaryTrack(); track != nullptr && track->timebase().isValid()) {
        return track->timebase();
    }
    return Rational{1, 1000};
}

const StreamPacketTrack* SplitPlanner::boundaryTrack() const {
    if (const StreamModel* video = model_.videoStream(); video != nullptr) {
        if (const StreamPacketTrack* track = index_.track(video->index); track != nullptr && !track->empty()) {
            return track;
        }
    }
    for (const auto& track : index_.tracks()) {
        if (!track.empty()) {
            return &track;
        }
    }
    return nullptr;
}

Rational SplitPlanner::sourceEnd() const {
    Rational end{0, 1};
    if (model_.durationResolved.known()) {
        end = *model_.durationResolved.seconds;
    }
    for (const auto& track : index_.tracks()) {
        if (auto last = track.lastEndTime(); last.has_value() && compareRational(*last, end) > 0) {
            end = *last;
        }
    }
    return end;
}

Rational SplitPlanner::frameDuration() const {
    if (const StreamModel* video = model_.videoStream();
        video != nullptr && video->baseFrameRate.isValid() && video->baseFrameRate.numerator > 0) {
        if (auto duration = divideRational(Rational{1, 1}, video->baseFrameRate); duration.has_value()) {
            return duration.value();
        }
    }
    return Rational{1, 25};
}

std::vector<Rational> SplitPlanner::candidateBoundaries() const {
    std::vector<Rational> candidates;
    const StreamPacketTrack* track = boundaryTrack();
    if (track == nullptr) {
        return candidates;
    }
    const bool copyStyle = constraint_.preservation == PreservationMode::StreamCopy ||
                           constraint_.preservation == PreservationMode::KeyframeAligned;
    const bool videoTrack =
        model_.videoStream() != nullptr && track->streamIndex() == model_.videoStream()->index;

    if (copyStyle && videoTrack) {
        candidates = track->keyframeTimes();
    } else if (copyStyle) {
        // Audio-only or no video: every packet is independently decodable enough
        // to start a part there.  The plan records this in its notes.
        candidates.reserve(track->size());
        for (const auto& packet : track->packets()) {
            if (packet.presentationSeconds.isValid() && packet.presentationSeconds.denominator != 0) {
                candidates.push_back(packet.presentationSeconds);
            }
        }
    } else {
        // Smart cut / re-encode: frame-accurate boundaries are possible, so every
        // video frame is a candidate.
        candidates.reserve(track->size());
        for (const auto& packet : track->packets()) {
            if (packet.presentationSeconds.isValid() && packet.presentationSeconds.denominator != 0) {
                candidates.push_back(packet.presentationSeconds);
            }
        }
    }

    // The binary search below (and every other consumer of the grid) needs the
    // candidates in ascending order.  Packets arrive in decode order, and with
    // B-frames their presentation timestamps are not monotonic, so sorting is not
    // optional; duplicates are removed because two packets cannot share a
    // boundary.
    std::sort(candidates.begin(), candidates.end(),
              [](const Rational& a, const Rational& b) { return compareRational(a, b) < 0; });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const Rational& a, const Rational& b) {
                                     return compareRational(a, b) == 0;
                                 }),
                     candidates.end());

    // The end of the source is always a valid end position: the last part simply
    // runs to the end of the file, so it needs no boundary inside the file.
    const Rational end = sourceEnd();
    if (candidates.empty() || compareRational(candidates.back(), end) < 0) {
        candidates.push_back(end);
    }

    if (!candidates.empty()) {
        stats_.gridStep = subtractRational(candidates.back(), candidates.front()).value_or(Rational{0, 1});
        if (stats_.gridStep.isPositive() && candidates.size() > 1) {
            stats_.gridStep = divideRational(stats_.gridStep, Rational{static_cast<std::int64_t>(candidates.size() - 1), 1})
                                  .value_or(Rational{0, 1});
        }
    }
    return candidates;
}

bool SplitPlanner::boundaryIsKeyframe(const Rational& time) const {
    const StreamPacketTrack* track = boundaryTrack();
    if (track == nullptr) {
        return false;
    }
    if (model_.videoStream() != nullptr && track->streamIndex() == model_.videoStream()->index) {
        return track->isKeyframeAt(time);
    }
    return true;  // non-video boundaries do not need a keyframe
}

Rational SplitPlanner::nextKeyframeAtOrAfter(const Rational& time) const {
    const StreamPacketTrack* track = boundaryTrack();
    if (track == nullptr) {
        return time;
    }
    if (auto keyframe = track->keyframeAtOrAfter(time); keyframe.has_value()) {
        return *keyframe;
    }
    return sourceEnd();
}

Rational SplitPlanner::keyframeAtOrBefore(const Rational& time) const {
    const StreamPacketTrack* track = boundaryTrack();
    if (track == nullptr) {
        return time;
    }
    if (auto keyframe = track->keyframeAtOrBefore(time); keyframe.has_value()) {
        return *keyframe;
    }
    return Rational{0, 1};
}

std::optional<Rational> SplitPlanner::nextCandidateAfter(const Rational& time) const {
    for (const Rational& candidate : candidateBoundaries()) {
        if (compareRational(candidate, time) > 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

ByteCount SplitPlanner::payloadInRange(const Rational& start, const Rational& end) const {
    ByteCount total = 0;
    const std::vector<const StreamModel*> streams = requiredStreams(constraint_, model_);
    for (const StreamModel* stream : streams) {
        if (const StreamPacketTrack* track = index_.track(stream->index); track != nullptr) {
            total = saturatingAdd(total, track->bytesInRange(start, end));
        }
    }
    return total;
}

ByteCount SplitPlanner::predictedFileBytes(ByteCount payload) const {
    return sizeModel_.predictFileBytes(payload);
}

ByteCount SplitPlanner::payloadBudget() const {
    if (!constraint_.hasSizeLimit()) {
        return kUnlimited;
    }
    return sizeModel_.maxPayloadForLimit(constraint_.size->maxBytes);
}

Rational SplitPlanner::durationLimitFor(const Rational& /*start*/) const {
    std::optional<Rational> limit;
    if (constraint_.hasDurationLimit()) {
        Rational value = constraint_.duration->maxSeconds;
        // The limit is used as typed here.  A *copied* part ends exactly at the
        // last packet that starts before the boundary, so its container duration
        // is the boundary itself and the limit can be met to the tick.  Only a
        // boundary that does not sit on the packet grid - which happens when the
        // end of a part is re-encoded - needs the one-frame safety margin, and
        // chooseEnd applies it there (settings_.subtractFrameDurationFromDurationLimit).
        if (compareRational(value, Rational{0, 1}) <= 0) {
            value = Rational{1, 1000};
        }
        limit = value;
    }
    if (constraint_.targetPartSeconds.has_value() && constraint_.targetPartSeconds->isPositive()) {
        if (!limit.has_value() || compareRational(*constraint_.targetPartSeconds, *limit) < 0) {
            limit = constraint_.targetPartSeconds;
        }
    }
    return limit.value_or(Rational{0, 0});
}

SplitPlanner::Choice SplitPlanner::chooseEnd(const Rational& start, const Rational& hardEnd,
                                             bool allowExactTime) const {
    Choice choice;
    const std::vector<Rational> candidates = candidateBoundaries();
    if (candidates.empty()) {
        return choice;
    }

    const ByteCount budget = payloadBudget();
    const Rational durationLimit = durationLimitFor(start);
    const bool hasDurationLimit = durationLimit.isPositive();

    // The position the limits alone would choose, before any snapping.
    Rational requested = hardEnd;
    BoundaryReason requestedReason = BoundaryReason::RemainingSource;

    // 1. Byte budget: binary search for the last candidate inside the budget.
    //    payloadInRange is monotone in the end position, so the search is valid.
    std::size_t low = 0;
    std::size_t high = candidates.size();
    std::size_t best = 0;
    bool haveBest = false;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        ++stats_.candidateEvaluations;
        const Rational candidate = candidates[middle];
        if (compareRational(candidate, start) <= 0) {
            low = middle + 1;
            continue;
        }
        const ByteCount payload = payloadInRange(start, candidate);
        if (constraint_.hasSizeLimit() && payload > budget) {
            high = middle;
        } else {
            best = middle;
            haveBest = true;
            low = middle + 1;
        }
    }
    if (!haveBest) {
        return choice;  // even the first candidate after the start is too large
    }

    Rational chosen = candidates[best];
    if (constraint_.hasSizeLimit() && best + 1 < candidates.size() &&
        payloadInRange(start, candidates[best + 1]) > budget) {
        requested = candidates[best + 1];
        requestedReason = BoundaryReason::SizeLimit;
    }

    // 2. Duration limit and target duration clamp the choice.
    if (hasDurationLimit) {
        Rational allowed = start;
        if (auto sum = addRational(start, durationLimit); sum.has_value()) {
            allowed = sum.value();
        }
        if (compareRational(chosen, allowed) > 0) {
            if (allowExactTime) {
                // The part will be re-encoded to end exactly here, so its last
                // frame can reach one frame beyond the boundary: keep that frame
                // inside the limit rather than overshooting it.
                Rational safe = allowed;
                if (settings_.subtractFrameDurationFromDurationLimit) {
                    if (auto adjusted = subtractRational(allowed, frameDuration()); adjusted.has_value()) {
                        safe = adjusted.value();
                    }
                }
                chosen = compareRational(safe, start) > 0 ? safe : allowed;
                allowed = chosen;
            } else {
                // Copy mode: snap back to the last boundary at or before the limit.
                chosen = start;
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    if (compareRational(candidates[index], allowed) <= 0 &&
                        compareRational(candidates[index], start) > 0) {
                        chosen = candidates[index];
                    } else if (compareRational(candidates[index], allowed) > 0) {
                        break;
                    }
                }
                if (compareRational(chosen, start) <= 0) {
                    return choice;
                }
            }
            if (compareRational(allowed, requested) < 0 || requestedReason == BoundaryReason::RemainingSource) {
                requested = allowed;
                requestedReason = constraint_.targetPartSeconds.has_value() &&
                                          (!constraint_.hasDurationLimit() ||
                                           compareRational(*constraint_.targetPartSeconds,
                                                           constraint_.duration->maxSeconds) <= 0)
                                      ? BoundaryReason::TargetDuration
                                      : BoundaryReason::DurationLimit;
            }
        }
    }

    // 3. Never pass the end of the source.
    if (compareRational(chosen, hardEnd) > 0) {
        chosen = hardEnd;
        if (!allowExactTime) {
            chosen = keyframeAtOrBefore(hardEnd);
        }
    }

    // 4. Never go backwards past the start.
    if (compareRational(chosen, start) <= 0) {
        return choice;
    }

    // 5. In the copy modes the boundary has to be a keyframe (or the source end).
    bool snapped = false;
    if (!allowExactTime && !boundaryIsKeyframe(chosen) && compareRational(chosen, hardEnd) < 0) {
        const Rational snappedBack = keyframeAtOrBefore(chosen);
        if (compareRational(snappedBack, start) > 0) {
            chosen = snappedBack;
            snapped = true;
            ++stats_.keyframeSnapped;
        } else {
            return choice;  // no usable keyframe inside the part
        }
    } else if (allowExactTime) {
        ++stats_.exactBoundaries;
    }

    const Rational actualPayload = Rational{0, 1};
    (void)actualPayload;

    choice.end = chosen;
    choice.requested = requested;
    choice.requestedIsMeaningful = requestedReason != BoundaryReason::RemainingSource && snapped;
    choice.reason = snapped ? BoundaryReason::KeyframeSearch : requestedReason;
    choice.requestedReason = requestedReason;
    choice.exact = !snapped && compareRational(chosen, start) > 0 &&
                   (allowExactTime || boundaryIsKeyframe(chosen));

    {
        std::string explanation;
        if (snapped) {
            if (auto delta = subtractRational(chosen, requested); delta.has_value()) {
                explanation = "the closest keyframe before the requested position is " + secondsText(chosen) +
                              "; the requested position was " + secondsText(requested) + " (" +
                              secondsText(delta.value()) + " later)";
            } else {
                explanation = "the boundary was moved back to the keyframe at " + secondsText(chosen);
            }
            if (requestedReason == BoundaryReason::SizeLimit) {
                explanation += ", because the next frame after it would push the part over the size limit";
            } else if (requestedReason == BoundaryReason::DurationLimit) {
                explanation += ", because the next frame after it would push the part over the duration limit";
            } else if (requestedReason == BoundaryReason::TargetDuration) {
                explanation += ", because the target part duration is reached there";
            }
            explanation += ".";
        } else if (requestedReason == BoundaryReason::SizeLimit) {
            explanation = "the size limit is reached after " + secondsText(chosen) +
                          "; the next frame would not fit";
        } else if (requestedReason == BoundaryReason::DurationLimit) {
            explanation = "the duration limit is reached at " + secondsText(chosen);
        } else if (requestedReason == BoundaryReason::TargetDuration) {
            explanation = "the target part duration is reached at " + secondsText(chosen);
        } else {
            explanation = "the rest of the file ends at " + secondsText(chosen);
        }
        choice.explanation = explanation;
    }
    choice.found = true;
    return choice;
}

SegmentPlan SplitPlanner::makeSegment(const SplitPlan& plan, const Rational& start, const Rational& end, int index,
                                      int total, const BoundaryPlan& boundary) const {
    SegmentPlan segment;
    segment.index = index;
    segment.total = total;
    segment.startSeconds = start;
    segment.endSeconds = end;
    if (auto duration = subtractRational(end, start); duration.has_value()) {
        segment.predictedDuration = duration.value();
    }
    segment.startBoundary = boundary;

    // Method and re-encode regions.
    switch (constraint_.preservation) {
        case PreservationMode::StreamCopy:
        case PreservationMode::KeyframeAligned:
            segment.method = SegmentMethod::Copy;
            break;
        case PreservationMode::SmartCut: {
            if (boundaryIsKeyframe(start) || index == 1) {
                segment.method = SegmentMethod::Copy;
            } else {
                const Rational headEnd = nextKeyframeAtOrAfter(start);
                if (compareRational(headEnd, end) >= 0) {
                    // The whole part lies inside one group of pictures, so the
                    // re-encoded head is the whole part.
                    segment.method = SegmentMethod::ReEncode;
                    segment.reencodeRegions.push_back(
                        ReencodeRegion{start, end, "the whole part lies inside one group of pictures"});
                } else {
                    segment.method = SegmentMethod::SmartCut;
                    segment.reencodeRegions.push_back(ReencodeRegion{
                        start, headEnd,
                        "the part starts between keyframes, so this region is re-encoded to keep the exact start"});
                }
            }
            break;
        }
        case PreservationMode::FullReEncode:
            segment.method = SegmentMethod::ReEncode;
            break;
    }

    const std::vector<const StreamModel*> streams = requiredStreams(constraint_, model_);
    for (const StreamModel* stream : streams) {
        const bool canBeEncoded = stream->kind == StreamKind::Video || stream->kind == StreamKind::Audio;
        const bool mustReencode =
            segment.method == SegmentMethod::ReEncode
                ? canBeEncoded
                : (segment.method == SegmentMethod::SmartCut && stream->index == plan.boundaryStreamIndex);
        if (mustReencode) {
            segment.reencodedStreamIndexes.push_back(stream->index);
            segment.reencodedCodecNames.push_back(stream->codecName);
            segment.reencodedKinds.push_back(stream->kind);
            segment.reencodedBitRates.push_back(stream->bitRate.has_value()
                                                    ? static_cast<int>(*stream->bitRate)
                                                    : 0);
        } else {
            segment.copiedStreamIndexes.push_back(stream->index);
        }
    }
    if (segment.method == SegmentMethod::ReEncode) {
        for (const StreamModel* stream : streams) {
            if (stream->kind == StreamKind::Subtitle || stream->kind == StreamKind::Attachment ||
                stream->kind == StreamKind::Data) {
                segment.notes.push_back("stream " + std::to_string(stream->index) + " (" +
                                        std::string(streamKindName(stream->kind)) + ", " + stream->codecName +
                                        ") cannot be re-encoded and is copied as it is");
            }
        }
    }

    segment.predictedPayloadBytes = payloadInRange(start, end);
    if (segment.method != SegmentMethod::Copy) {
        // Re-encoded regions are predicted from the source payload of the same
        // range; this is an estimate and the plan says so.
        segment.notes.push_back("the size of the re-encoded region is estimated from the source bit rate and is "
                                "verified after the part is written");
    }
    segment.predictedOverheadBytes = sizeModel_.overheadAllowance(segment.predictedPayloadBytes);
    segment.predictedFileBytes = saturatingAdd(segment.predictedPayloadBytes, segment.predictedOverheadBytes);

    // Output file name.
    NamingContext context;
    context.sourceStem = std::filesystem::path(plan.sourcePath).stem().string();
    context.extension = plan.outputExtension;
    context.index = index;
    context.total = total;
    context.startSeconds = start;
    context.endSeconds = end;
    context.duration = segment.predictedDuration;
    context.limitBytes = constraint_.hasSizeLimit() ? constraint_.size->maxBytes : 0;
    context.method = segmentMethodName(segment.method);
    if (auto name = renderOutputName(naming_, context); name.has_value()) {
        segment.outputFileName = name.value();
    } else {
        segment.outputFileName = context.sourceStem + " - part " + std::to_string(index) + "." +
                                 plan.outputExtension;
        segment.notes.push_back("the naming pattern could not be used (" + name.error().message +
                                "), so the default name was applied");
    }
    return segment;
}

Result<SplitPlan> SplitPlanner::buildPlan(const std::string& outputDirectory, int firstIndex, Rational start,
                                          std::size_t keepParts, const SplitPlan* previous) {
    SplitPlan plan;
    plan.planned = false;
    plan.sourcePath = model_.path;
    plan.sourceFileBytes = model_.fileSize;
    plan.sourceFormatNames = model_.formatNames;
    plan.sourceDurationKnown = model_.durationResolved.known();
    plan.sourceDurationExact = model_.durationResolved.source == DurationSource::Container ||
                               model_.durationResolved.source == DurationSource::StreamTimeline;
    plan.sourceDuration = model_.durationResolved.seconds.value_or(sourceEnd());
    plan.constraint = constraint_;
    plan.outputDirectory = outputDirectory;
    plan.timebase = gridTimebase();
    plan.sizeModel = sizeModel_;
    plan.outputExtension = extensionForFormat(model_.formatNames, model_.path);
    plan.sourceSignatures = signaturesOf(model_);
    if (model_.videoStream() != nullptr) {
        plan.boundaryStreamIndex = model_.videoStream()->index;
    } else if (const StreamPacketTrack* track = boundaryTrack(); track != nullptr) {
        plan.boundaryStreamIndex = track->streamIndex();
    }

    if (previous != nullptr && keepParts > 0) {
        plan.warnings = previous->warnings;
        plan.notes = previous->notes;
        for (std::size_t i = 0; i < keepParts && i < previous->segments.size(); ++i) {
            plan.segments.push_back(previous->segments[i]);
        }
    }

    const Rational end = sourceEnd();
    const Rational firstStartOfPlan = start;
    if (compareRational(start, end) >= 0) {
        if (plan.segments.empty()) {
            plan.failureExplanation = "The source is empty or the first boundary is at the end of the file.";
            return plan;
        }
        plan.planned = true;
        return plan;
    }

    if (candidateBoundaries().empty()) {
        plan.failureExplanation =
            "No usable packet timestamps were found in this file, so MonoSect cannot place a boundary. "
            "The file may be damaged, or it may be a format ffprobe cannot index.";
        return plan;
    }

    int index = firstIndex;
    int failures = 0;
    while (compareRational(start, end) < 0) {
        if (index > settings_.maximumParts) {
            plan.failureExplanation = "The plan would need more than " + std::to_string(settings_.maximumParts) +
                                      " parts; such a job is refused rather than started.";
            return plan;
        }
        const bool allowExactTime = constraint_.preservation == PreservationMode::SmartCut ||
                                    constraint_.preservation == PreservationMode::FullReEncode;
        Choice choice = chooseEnd(start, end, allowExactTime);
        if (!choice.found) {
            // Explain precisely why no boundary works from here.
            const std::optional<Rational> nextBoundary = nextCandidateAfter(start);
            std::string explanation;
            if (!nextBoundary.has_value()) {
                explanation = "no boundary exists after " + secondsText(start) +
                              " in the packet index, so there is nothing left to write.";
            } else if (constraint_.hasSizeLimit() &&
                       predictedFileBytes(payloadInRange(start, *nextBoundary)) > constraint_.size->maxBytes) {
                explanation = "the smallest part this source can produce from " + secondsText(start) + " is " +
                              formatDuration(subtractRational(*nextBoundary, start).value_or(Rational{0, 1})) +
                              " long and ends at " + secondsText(*nextBoundary) + ". Its estimated size is " +
                              formatBytesShort(predictedFileBytes(payloadInRange(start, *nextBoundary))) +
                              ", already more than the limit of " + formatBytesShort(constraint_.size->maxBytes) +
                              ". The estimate is derived from the source's own container overhead and would be "
                              "replaced by a measured value after the first written part, but no part can be "
                              "written inside this limit. Raise the limit.";
            } else if (constraint_.hasDurationLimit()) {
                explanation = "no position after " + secondsText(start) +
                              " satisfies the duration limit of " +
                              formatDuration(constraint_.duration->maxSeconds) + ".";
            } else {
                explanation = "no usable boundary was found after " + secondsText(start) + ".";
            }
            plan.failureExplanation = explanation;
            return plan;
        }

        // Keep the tail: when the remainder is shorter than the minimum part
        // duration, extend this part to the end of the file if that is safe.
        Rational chosenEnd = choice.end;
        if (auto remaining = subtractRational(end, chosenEnd); remaining.has_value() &&
            compareRational(remaining.value(), settings_.minimumPartDuration) < 0 &&
            compareRational(chosenEnd, end) < 0) {
            const ByteCount payload = payloadInRange(start, end);
            const bool sizeOk = !constraint_.hasSizeLimit() ||
                                predictedFileBytes(payload) <= constraint_.size->maxBytes;
            bool durationOk = true;
            if (constraint_.hasDurationLimit()) {
                const Rational total = subtractRational(end, start).value_or(Rational{0, 1});
                durationOk = compareRational(total, constraint_.duration->maxSeconds) <= 0;
            }
            if (sizeOk && durationOk) {
                chosenEnd = end;
                choice.reason = BoundaryReason::RemainingSource;
                choice.explanation = "the remainder of the file is shorter than the minimum part length, so it is "
                                     "kept in this part";
            }
        }

        const Rational segmentDuration = subtractRational(chosenEnd, start).value_or(Rational{0, 1});
        if (compareRational(segmentDuration, settings_.minimumPartDuration) < 0 &&
            compareRational(chosenEnd, end) < 0) {
            // The only boundary that fits is very short.  If a slightly larger
            // part is still inside the hard limits, prefer it; otherwise report.
            const Rational forced = nextKeyframeAtOrAfter(chosenEnd);
            const ByteCount payload = payloadInRange(start, forced);
            const bool safe = (!constraint_.hasSizeLimit() ||
                               predictedFileBytes(payload) <= constraint_.size->maxBytes) &&
                              (!constraint_.hasDurationLimit() ||
                               compareRational(subtractRational(forced, start).value_or(Rational{0, 1}),
                                               constraint_.duration->maxSeconds) <= 0);
            if (safe && compareRational(forced, end) < 0) {
                chosenEnd = forced;
                choice.reason = BoundaryReason::MinimumPartSize;
                choice.explanation = "the part was extended to the next keyframe because the size limit alone "
                                     "would have produced a part shorter than the minimum length";
            } else {
                plan.warnings.push_back("part " + std::to_string(index) + " is shorter than the minimum part "
                                        "length (" + formatDuration(segmentDuration) +
                                        "); a hard limit leaves no larger choice");
            }
        }

        BoundaryPlan boundary;
        boundary.actualSeconds = start;
        boundary.explanation = choice.explanation;
        boundary.reason = choice.reason;
        boundary.exact = choice.exact;
        if (index == firstIndex && compareRational(start, firstStartOfPlan) == 0) {
            boundary.requested = false;
            boundary.requestedSeconds = start;
            boundary.delta = Rational{0, 1};
            boundary.reason = BoundaryReason::SourceStart;
            boundary.exact = true;
            boundary.explanation = "part 1 starts at the beginning of the file";
        } else {
            boundary.requested = true;
            boundary.requestedSeconds = choice.requested;
            boundary.delta = subtractRational(start, choice.requested).value_or(Rational{0, 1});
            boundary.explanation = choice.explanation;
        }

        SegmentPlan segment = makeSegment(plan, start, chosenEnd, index, /*total*/ 0, boundary);
        segment.endsAtSourceEnd = compareRational(chosenEnd, end) >= 0;
        if (segment.method != SegmentMethod::Copy && segment.startBoundary.requested) {
            segment.startBoundary.explanation =
                "the part begins at the exact position because the head of the part is re-encoded; " +
                segment.startBoundary.explanation;
        }
        plan.segments.push_back(segment);
        start = chosenEnd;
        ++index;
        ++failures;
        if (failures > settings_.maximumParts) {
            plan.failureExplanation = "The planner did not make progress; this is a bug.";
            return plan;
        }
    }

    for (auto& segment : plan.segments) {
        segment.total = static_cast<int>(plan.segments.size());
    }
    plan.predictedTotalBytes = 0;
    for (const auto& segment : plan.segments) {
        plan.predictedTotalBytes = saturatingAdd(plan.predictedTotalBytes, segment.predictedFileBytes);
    }
    plan.predictedTotalDuration = Rational{0, 1};
    for (const auto& segment : plan.segments) {
        if (auto total = addRational(plan.predictedTotalDuration, segment.predictedDuration); total.has_value()) {
            plan.predictedTotalDuration = total.value();
        }
    }

    // The plan is only marked planned when the hard limits hold for every part.
    if (!plan.respectsHardSizeLimit()) {
        plan.failureExplanation = "This plan would create a part larger than the hard size limit; it is refused.";
        return plan;
    }
    if (!plan.respectsHardDurationLimit()) {
        plan.failureExplanation =
            "This plan would create a part longer than the hard duration limit; it is refused.";
        return plan;
    }

    // A part-count limit is a hard limit too: a job that would need more parts
    // than the user allowed is refused here, before anything is written, with the
    // numbers that show what would be needed (TODO 061, 074).
    if (constraint_.maxParts.has_value() && !plan.segments.empty() &&
        static_cast<int>(plan.segments.size()) > *constraint_.maxParts) {
        const ByteCount largest = plan.largestPredictedBytes();
        plan.failureExplanation =
            "The limits need " + std::to_string(plan.segments.size()) + " parts but at most " +
            std::to_string(*constraint_.maxParts) + " were allowed" +
            (largest > 0 ? " (the largest part that fits is " +
                               (constraint_.hasSizeLimit() ? formatBytesShort(largest)
                                                           : formatDuration(plan.longestPredictedDuration())) +
                               ")"
                        : "") +
            ". Raise the part count or the limit so that fewer, larger parts are enough; MonoSect will not "
            "publish a part that breaks a limit.";
        return plan;
    }

    plan.notes.push_back(stats_.gridDescription);
    if (!constraint_.hasSizeLimit()) {
        plan.notes.push_back("no size limit was given; parts follow the duration limit");
    } else {
        plan.notes.push_back("size model: " + sizeModel_.explanation());
    }
    if (constraint_.preservation == PreservationMode::StreamCopy ||
        constraint_.preservation == PreservationMode::KeyframeAligned) {
        plan.notes.push_back("every packet is copied; boundaries are existing keyframes of the source");
    }
    plan.planned = true;
    stats_.parts = static_cast<int>(plan.segments.size());
    return plan;
}

Result<SplitPlan> SplitPlanner::plan(const std::string& outputDirectory) {
    stats_ = PlannerStats{};
    Rational start{0, 1};
    if (const StreamPacketTrack* track = boundaryTrack(); track != nullptr) {
        if (auto first = track->firstPresentationTime(); first.has_value() &&
                                                         compareRational(*first, Rational{0, 1}) > 0) {
            start = *first;
        }
    }
    if (!model_.durationResolved.known() && !constraint_.hasDurationLimit()) {
        // Sizing without a known duration is still possible: the packet index
        // carries the real end of the last packet.
    }
    auto result = buildPlan(outputDirectory, 1, start, 0, nullptr);
    if (result.has_value()) {
        result.value().notes.push_back(stats_.gridDescription);
        if (stats_.keyframeSnapped > 0) {
            result.value().notes.push_back(std::to_string(stats_.keyframeSnapped) +
                                           " boundary(ies) were placed on the closest keyframe instead of the "
                                           "position the limit alone would choose");
        }
    }
    return result;
}

Result<SplitPlan> SplitPlanner::replanRemaining(const SplitPlan& previous, std::size_t completedParts) {
    stats_ = PlannerStats{};
    if (completedParts >= previous.segments.size()) {
        SplitPlan plan = previous;
        plan.sizeModel = sizeModel_;
        return plan;
    }
    const Rational start = previous.segments[completedParts].startSeconds;
    auto result = buildPlan(previous.outputDirectory, static_cast<int>(completedParts) + 1, start,
                            completedParts, &previous);
    if (!result.has_value()) {
        return result;
    }
    SplitPlan& plan = result.value();
    // Changing parts that were already written would invalidate their record.
    for (std::size_t i = 0; i < completedParts && i < previous.segments.size(); ++i) {
        if (i >= plan.segments.size()) {
            plan.segments.push_back(previous.segments[i]);
        } else {
            plan.segments[i] = previous.segments[i];
        }
    }
    std::sort(plan.segments.begin(), plan.segments.end(),
              [](const SegmentPlan& a, const SegmentPlan& b) { return a.index < b.index; });
    for (auto& segment : plan.segments) {
        segment.total = static_cast<int>(plan.segments.size());
    }
    plan.planned = true;
    plan.sizeModel = sizeModel_;
    return plan;
}

Result<SegmentPlan> SplitPlanner::planOnePart(const SplitPlan& reference, Rational startSeconds, int index,
                                              int totalHint, const std::string& outputDirectory) {
    stats_ = PlannerStats{};
    const Rational end = sourceEnd();
    if (compareRational(startSeconds, end) >= 0) {
        return Error{ErrorCode::PlanningFailed, "There is nothing left to plan from this position.",
                     "position: " + secondsText(startSeconds)};
    }
    const bool allowExactTime = constraint_.preservation == PreservationMode::SmartCut ||
                                constraint_.preservation == PreservationMode::FullReEncode;
    Choice choice = chooseEnd(startSeconds, end, allowExactTime);
    if (!choice.found) {
        return Error{ErrorCode::ConstraintUnsatisfiable,
                     "No boundary after this position satisfies the hard limits.",
                     "position: " + secondsText(startSeconds),
                     "Raise the size limit, raise the duration limit, or allow a mode that can re-encode."};
    }
    SplitPlan shell = reference;
    shell.outputDirectory = outputDirectory;
    BoundaryPlan boundary;
    boundary.requested = true;
    boundary.requestedSeconds = choice.requested;
    boundary.actualSeconds = startSeconds;
    boundary.delta = subtractRational(startSeconds, choice.requested).value_or(Rational{0, 1});
    boundary.exact = choice.exact;
    boundary.reason = choice.reason;
    boundary.explanation = choice.explanation;
    return makeSegment(shell, startSeconds, choice.end, index, totalHint, boundary);
}

} // namespace monosect::core
