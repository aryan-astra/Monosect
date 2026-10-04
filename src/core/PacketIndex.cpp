#include "core/PacketIndex.hpp"

#include "core/Json.hpp"
#include "core/MediaModel.hpp"

#include <algorithm>
#include <numeric>

namespace monosect::core {
namespace {

std::uint64_t magnitudeOf(std::int64_t value) noexcept {
    return value < 0 ? (~static_cast<std::uint64_t>(value) + 1u) : static_cast<std::uint64_t>(value);
}

std::int64_t gcd64(std::uint64_t a, std::uint64_t b) {
    while (b != 0) {
        const std::uint64_t tmp = a % b;
        a = b;
        b = tmp;
    }
    return static_cast<std::int64_t>(a);
}

/// ticks * timebase, exactly, or nullopt when the product does not fit.
std::optional<Rational> ticksToSeconds(std::int64_t ticks, const Rational& timebase) {
    if (!timebase.isValid() || timebase.numerator == 0) {
        return std::nullopt;
    }
    const std::uint64_t divisor =
        static_cast<std::uint64_t>(std::max<std::int64_t>(1, gcd64(magnitudeOf(ticks), magnitudeOf(timebase.denominator))));
    const std::int64_t reducedTicks = ticks / static_cast<std::int64_t>(divisor);
    const std::int64_t reducedDenominator = timebase.denominator / static_cast<std::int64_t>(divisor);
    const WideProduct product = wideMultiply(reducedTicks, timebase.numerator);
    if (product.high != 0) {
        return std::nullopt;
    }
    const std::int64_t numerator =
        product.negative ? -static_cast<std::int64_t>(product.low) : static_cast<std::int64_t>(product.low);
    return Rational{numerator, reducedDenominator};
}

/// Reads the ffprobe `flags` field ("K_", "K_D", "_D", ...).
std::uint8_t parseFlagsText(const std::string& text) {
    std::uint8_t flags = 0;
    for (const char c : text) {
        switch (c) {
            case 'K': flags |= static_cast<std::uint8_t>(PacketFlag::Keyframe); break;
            case 'D': flags |= static_cast<std::uint8_t>(PacketFlag::Discard); break;
            case 'C': flags |= static_cast<std::uint8_t>(PacketFlag::Corrupt); break;
            default: break;
        }
    }
    return flags;
}

} // namespace

void StreamPacketTrack::add(PacketRecord record) {
    finalized_ = false;
    if (!record.presentationSeconds.isValid() || record.presentationSeconds.denominator == 0) {
        ++untimedPackets_;
    }
    if (record.hasPts && record.hasDts && record.ptsTicks != record.dtsTicks) {
        hasReordering_ = true;
    }
    totalPayloadBytes_ += record.sizeBytes;
    packets_.push_back(std::move(record));
}

void StreamPacketTrack::finalize() {
    order_.clear();
    order_.reserve(packets_.size());
    for (std::uint32_t i = 0; i < packets_.size(); ++i) {
        const PacketRecord& packet = packets_[i];
        if (packet.presentationSeconds.isValid() && packet.presentationSeconds.denominator != 0) {
            order_.push_back(i);
        }
    }
    std::stable_sort(order_.begin(), order_.end(), [this](std::uint32_t a, std::uint32_t b) {
        const int comparison =
            compareRational(packets_[a].presentationSeconds, packets_[b].presentationSeconds);
        if (comparison != 0) {
            return comparison < 0;
        }
        return a < b;
    });

    cumulativeBytes_.assign(order_.size() + 1, 0);
    for (std::size_t i = 0; i < order_.size(); ++i) {
        cumulativeBytes_[i + 1] = cumulativeBytes_[i] + packets_[order_[i]].sizeBytes;
    }

    keyframesSorted_.clear();
    for (std::uint32_t index : order_) {
        if (packets_[index].isKeyframe()) {
            keyframesSorted_.push_back(index);
        }
    }
    finalized_ = true;
}

std::size_t StreamPacketTrack::lowerBoundByPresentationTime(const Rational& t) const {
    std::size_t low = 0;
    std::size_t high = order_.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (compareRational(packets_[order_[middle]].presentationSeconds, t) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

std::int64_t StreamPacketTrack::bytesBefore(const Rational& t) const {
    if (cumulativeBytes_.empty()) {
        return 0;
    }
    return cumulativeBytes_[lowerBoundByPresentationTime(t)];
}

std::int64_t StreamPacketTrack::bytesInRange(const Rational& t0, const Rational& t1) const {
    if (compareRational(t1, t0) <= 0) {
        return 0;
    }
    return bytesBefore(t1) - bytesBefore(t0);
}

std::int64_t StreamPacketTrack::packetsInRange(const Rational& t0, const Rational& t1) const {
    const std::size_t first = lowerBoundByPresentationTime(t0);
    const std::size_t last = lowerBoundByPresentationTime(t1);
    return static_cast<std::int64_t>(last - first);
}

std::optional<Rational> StreamPacketTrack::firstPresentationTime() const {
    if (order_.empty()) {
        return std::nullopt;
    }
    return packets_[order_.front()].presentationSeconds;
}

std::optional<Rational> StreamPacketTrack::lastPresentationTime() const {
    if (order_.empty()) {
        return std::nullopt;
    }
    return packets_[order_.back()].presentationSeconds;
}

std::optional<Rational> StreamPacketTrack::lastEndTime() const {
    if (order_.empty()) {
        return std::nullopt;
    }
    const PacketRecord& last = packets_[order_.back()];
    if (last.endSeconds.isValid() && last.endSeconds.denominator != 0 &&
        compareRational(last.endSeconds, last.presentationSeconds) >= 0) {
        return last.endSeconds;
    }
    return last.presentationSeconds;
}

std::vector<Rational> StreamPacketTrack::keyframeTimes() const {
    std::vector<Rational> times;
    times.reserve(keyframesSorted_.size());
    for (const std::uint32_t index : keyframesSorted_) {
        times.push_back(packets_[index].presentationSeconds);
    }
    return times;
}

std::vector<std::int64_t> StreamPacketTrack::keyframeTicks() const {
    std::vector<std::int64_t> ticks;
    ticks.reserve(keyframesSorted_.size());
    for (const std::uint32_t index : keyframesSorted_) {
        ticks.push_back(packets_[index].ptsTicks);
    }
    return ticks;
}

std::optional<Rational> StreamPacketTrack::keyframeAtOrBefore(const Rational& t) const {
    std::optional<Rational> best;
    for (const std::uint32_t index : keyframesSorted_) {
        const Rational& time = packets_[index].presentationSeconds;
        if (compareRational(time, t) <= 0) {
            best = time;
        } else {
            break;  // keyframesSorted_ is in presentation order
        }
    }
    return best;
}

std::optional<Rational> StreamPacketTrack::keyframeAtOrAfter(const Rational& t) const {
    for (const std::uint32_t index : keyframesSorted_) {
        const Rational& time = packets_[index].presentationSeconds;
        if (compareRational(time, t) >= 0) {
            return time;
        }
    }
    return std::nullopt;
}

bool StreamPacketTrack::isKeyframeAt(const Rational& t) const {
    for (const std::uint32_t index : keyframesSorted_) {
        const int comparison = compareRational(packets_[index].presentationSeconds, t);
        if (comparison == 0) {
            return true;
        }
        if (comparison > 0) {
            break;
        }
    }
    return false;
}

void PacketIndex::addTrack(StreamPacketTrack track) {
    tracks_.push_back(std::move(track));
}

void PacketIndex::finalize() {
    for (auto& track : tracks_) {
        track.finalize();
    }
    std::sort(tracks_.begin(), tracks_.end(), [](const StreamPacketTrack& a, const StreamPacketTrack& b) {
        return a.streamIndex() < b.streamIndex();
    });
}

const StreamPacketTrack* PacketIndex::track(int streamIndex) const {
    for (const auto& track : tracks_) {
        if (track.streamIndex() == streamIndex) {
            return &track;
        }
    }
    return nullptr;
}

std::vector<const StreamPacketTrack*> PacketIndex::tracksOfKind(bool wantVideo) const {
    // The caller decides which streams matter; this helper only splits the video
    // tracks out by looking at the model-independent heuristics the caller passed
    // in.  The planner uses the media model instead.
    std::vector<const StreamPacketTrack*> out;
    for (const auto& track : tracks_) {
        if (wantVideo == (track.streamIndex() >= 0 && track.hasReordering())) {
            out.push_back(&track);
        }
    }
    return out;
}

std::int64_t PacketIndex::totalPayloadBytes() const noexcept {
    std::int64_t total = 0;
    for (const auto& track : tracks_) {
        total += track.totalPayloadBytes();
    }
    return total;
}

std::int64_t PacketIndex::totalPayloadBytesInRange(const Rational& t0, const Rational& t1) const {
    std::int64_t total = 0;
    for (const auto& track : tracks_) {
        total += track.bytesInRange(t0, t1);
    }
    return total;
}

std::size_t PacketIndex::totalPacketCount() const noexcept {
    std::size_t count = 0;
    for (const auto& track : tracks_) {
        count += track.size();
    }
    return count;
}

std::size_t PacketIndex::untimedPacketCount() const noexcept {
    std::size_t count = 0;
    for (const auto& track : tracks_) {
        count += track.untimedPacketCount();
    }
    return count;
}

std::int64_t PacketIndex::lowestFilePosition() const {
    std::int64_t lowest = -1;
    for (const auto& track : tracks_) {
        for (const auto& packet : track.packets()) {
            if (packet.filePosition < 0) {
                continue;
            }
            if (lowest < 0 || packet.filePosition < lowest) {
                lowest = packet.filePosition;
            }
        }
    }
    return lowest;
}

Result<PacketIndex> buildPacketIndex(const JsonValue& packetsArray, const ContainerModel& model,
                                     std::size_t* packetsWithoutTimestamp) {
    if (!packetsArray.isArray()) {
        return Error{ErrorCode::ProbeFailed, "ffprobe did not return a packet list.",
                     "the `packets` member is missing or is not an array",
                     "This is a bug in MonoSect or an unexpected ffprobe version."};
    }
    if (!packetsArray.items().empty() && model.streams.empty()) {
        return Error{ErrorCode::ProbeFailed, "Packet data was returned for an unknown stream set.",
                     "the media model has no streams",
                     "This is a bug in MonoSect or an unexpected ffprobe version."};
    }

    // One track per stream: a stream without packets stays visible as an empty
    // track, which is a fact the planner has to know about.
    std::vector<StreamPacketTrack> tracks;
    tracks.reserve(model.streams.size());
    std::vector<std::size_t> trackPositionByStreamIndex;
    for (const auto& stream : model.streams) {
        tracks.emplace_back(stream.index, stream.timebase);
    }
    const auto positionOf = [&model](int streamIndex) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < model.streams.size(); ++i) {
            if (model.streams[i].index == streamIndex) {
                return i;
            }
        }
        return std::nullopt;
    };

    std::size_t withoutTimestamp = 0;
    std::size_t unplaceable = 0;
    std::size_t unknownStreamPackets = 0;

    for (const auto& element : packetsArray.items()) {
        if (!element.isObject()) {
            continue;
        }
        const JsonValue* streamIndexValue = element.find("stream_index");
        if (streamIndexValue == nullptr) {
            continue;
        }
        const auto streamIndex = streamIndexValue->asInt64();
        if (!streamIndex.has_value()) {
            continue;
        }
        const auto position = positionOf(static_cast<int>(*streamIndex));
        if (!position.has_value()) {
            ++unknownStreamPackets;
            continue;
        }
        const StreamModel& stream = model.streams[*position];

        PacketRecord record;
        record.streamIndex = static_cast<int>(*streamIndex);
        if (const JsonValue* size = element.find("size"); size != nullptr) {
            if (auto value = size->asInt64(); value.has_value()) {
                record.sizeBytes = *value;
            }
        }
        if (const JsonValue* pts = element.find("pts"); pts != nullptr) {
            if (auto value = pts->asInt64(); value.has_value()) {
                record.ptsTicks = *value;
                record.hasPts = true;
            }
        }
        if (const JsonValue* dts = element.find("dts"); dts != nullptr) {
            if (auto value = dts->asInt64(); value.has_value()) {
                record.dtsTicks = *value;
                record.hasDts = true;
            }
        }
        if (const JsonValue* duration = element.find("duration"); duration != nullptr) {
            if (auto value = duration->asInt64(); value.has_value()) {
                record.durationTicks = *value;
                record.hasDuration = true;
            }
        }
        if (const JsonValue* positionValue = element.find("pos"); positionValue != nullptr) {
            if (auto value = positionValue->asInt64(); value.has_value()) {
                record.filePosition = *value;
            }
        }
        if (const JsonValue* flags = element.find("flags"); flags != nullptr) {
            if (auto text = flags->asString(); text.has_value()) {
                record.flags = parseFlagsText(*text);
            }
        }

        const bool haveTimestamp = record.hasPts || record.hasDts;
        const std::int64_t referenceTicks = record.hasPts ? record.ptsTicks : record.dtsTicks;
        std::optional<Rational> seconds;
        if (haveTimestamp && stream.timebaseKnown) {
            seconds = ticksToSeconds(referenceTicks, stream.timebase);
        }
        if (!seconds.has_value()) {
            ++withoutTimestamp;
            // The packet keeps its byte count but is marked as unplaceable so no
            // time range ever counts it.  The caller reports the number.
            record.presentationSeconds = Rational{0, 0};
            record.endSeconds = Rational{0, 0};
            tracks[*position].add(std::move(record));
            continue;
        }

        record.presentationSeconds = *seconds;
        record.endSeconds = *seconds;
        if (record.hasDuration && record.durationTicks > 0) {
            if (auto durationSeconds = ticksToSeconds(record.durationTicks, stream.timebase);
                durationSeconds.has_value()) {
                if (auto end = addRational(*seconds, *durationSeconds); end.has_value()) {
                    record.endSeconds = end.value();
                }
            }
        }
        if (!record.endSeconds.isValid() || record.endSeconds.denominator == 0) {
            record.endSeconds = *seconds;
        }
        tracks[*position].add(std::move(record));
    }

    PacketIndex index;
    for (auto& track : tracks) {
        index.addTrack(std::move(track));
    }
    index.finalize();

    if (packetsWithoutTimestamp != nullptr) {
        *packetsWithoutTimestamp = withoutTimestamp;
    }
    (void)unplaceable;
    (void)unknownStreamPackets;
    return index;
}

} // namespace monosect::core
