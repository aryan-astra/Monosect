// MonoSect — the packet and keyframe index.
//
// The index is the only source of truth for "how many bytes are in this time
// range" and "where are the keyframes".  It is built from
// `ffprobe -show_packets`, which reports the compressed size and the file
// position of every packet; that makes byte-exact planning possible without
// decoding anything.
//
// Two things this index deliberately does NOT do:
//  * It does not assume that `pts` is always present.  Packets without a usable
//    timestamp are kept, counted and reported, never silently dropped.
//  * It does not assume that packets arrive in presentation order.  They arrive
//    in decode order, so every query sorts by presentation time first.
#pragma once

#include "core/Rational.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

enum class PacketFlag : std::uint8_t {
    None = 0,
    Keyframe = 1u << 0,
    Discard = 1u << 1,
    Corrupt = 1u << 2,
};

[[nodiscard]] constexpr std::uint8_t operator|(PacketFlag a, PacketFlag b) noexcept {
    return static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b);
}

struct PacketRecord {
    int streamIndex = -1;
    std::int64_t ptsTicks = 0;
    std::int64_t dtsTicks = 0;
    std::int64_t durationTicks = 0;
    std::int64_t sizeBytes = 0;
    std::int64_t filePosition = -1;   // -1 when ffprobe did not report `pos`
    std::uint8_t flags = 0;
    bool hasPts = false;
    bool hasDts = false;
    bool hasDuration = false;

    /// Presentation time in seconds, exact.  Falls back to dts when the packet
    /// carries no pts (some subtitle streams); empty when neither is present.
    Rational presentationSeconds;
    /// End of the packet's presentation interval, or the same as the start when
    /// no duration is known.
    Rational endSeconds;

    [[nodiscard]] bool isKeyframe() const noexcept {
        return (flags & static_cast<std::uint8_t>(PacketFlag::Keyframe)) != 0;
    }
    [[nodiscard]] bool isDiscard() const noexcept {
        return (flags & static_cast<std::uint8_t>(PacketFlag::Discard)) != 0;
    }
};

/// All packets of one stream, plus the derived tables used by the planner.
class StreamPacketTrack {
public:
    StreamPacketTrack() = default;
    StreamPacketTrack(int streamIndex, Rational timebase) : streamIndex_(streamIndex), timebase_(timebase) {}

    [[nodiscard]] int streamIndex() const noexcept { return streamIndex_; }
    [[nodiscard]] const Rational& timebase() const noexcept { return timebase_; }
    [[nodiscard]] bool empty() const noexcept { return packets_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return packets_.size(); }
    [[nodiscard]] const std::vector<PacketRecord>& packets() const noexcept { return packets_; }
    [[nodiscard]] std::int64_t totalPayloadBytes() const noexcept { return totalPayloadBytes_; }

    void add(PacketRecord record);
    /// Sorts by presentation time and builds the cumulative tables.  Must be
    /// called after all packets were added.
    void finalize();

    /// Payload bytes of packets whose presentation time is strictly before `t`.
    [[nodiscard]] std::int64_t bytesBefore(const Rational& t) const;
    /// Payload bytes of packets with `t0 <= pts < t1`.
    [[nodiscard]] std::int64_t bytesInRange(const Rational& t0, const Rational& t1) const;
    /// Number of packets with `t0 <= pts < t1`.
    [[nodiscard]] std::int64_t packetsInRange(const Rational& t0, const Rational& t1) const;

    /// Presentation time of the first/last packet; empty when there is none.
    [[nodiscard]] std::optional<Rational> firstPresentationTime() const;
    [[nodiscard]] std::optional<Rational> lastPresentationTime() const;
    /// End of the last packet's presentation interval.
    [[nodiscard]] std::optional<Rational> lastEndTime() const;

    [[nodiscard]] std::vector<Rational> keyframeTimes() const;
    [[nodiscard]] std::vector<std::int64_t> keyframeTicks() const;
    [[nodiscard]] std::optional<Rational> keyframeAtOrBefore(const Rational& t) const;
    [[nodiscard]] std::optional<Rational> keyframeAtOrAfter(const Rational& t) const;
    [[nodiscard]] bool isKeyframeAt(const Rational& t) const;
    [[nodiscard]] std::size_t keyframeCount() const noexcept { return keyframesSorted_.size(); }

    /// True when the stream has packets where dts differs from pts, which means
    /// a copy that starts mid-stream must start at a keyframe to be decodable.
    [[nodiscard]] bool hasReordering() const noexcept { return hasReordering_; }
    /// Packets that carry no usable presentation or decode timestamp.
    [[nodiscard]] std::size_t untimedPacketCount() const noexcept { return untimedPackets_; }

private:
    [[nodiscard]] std::size_t lowerBoundByPresentationTime(const Rational& t) const;

    int streamIndex_ = -1;
    Rational timebase_{1, 1};
    std::vector<PacketRecord> packets_;
    // Indices into packets_, ordered by presentation time, with cumulative bytes.
    std::vector<std::uint32_t> order_;
    std::vector<std::int64_t> cumulativeBytes_;
    std::vector<std::uint32_t> keyframesSorted_;
    std::int64_t totalPayloadBytes_ = 0;
    std::size_t untimedPackets_ = 0;
    bool hasReordering_ = false;
    bool finalized_ = false;
};

class PacketIndex {
public:
    [[nodiscard]] bool empty() const noexcept { return tracks_.empty(); }
    [[nodiscard]] std::size_t streamCount() const noexcept { return tracks_.size(); }

    void addTrack(StreamPacketTrack track);
    void finalize();

    [[nodiscard]] const StreamPacketTrack* track(int streamIndex) const;
    [[nodiscard]] std::vector<const StreamPacketTrack*> tracksOfKind(bool wantVideo) const;
    [[nodiscard]] const std::vector<StreamPacketTrack>& tracks() const noexcept { return tracks_; }

    [[nodiscard]] std::int64_t totalPayloadBytes() const noexcept;
    [[nodiscard]] std::int64_t totalPayloadBytesInRange(const Rational& t0, const Rational& t1) const;
    [[nodiscard]] std::size_t totalPacketCount() const noexcept;
    [[nodiscard]] std::size_t untimedPacketCount() const noexcept;
    /// Lowest packet position, or -1.  Used to compute how much container
    /// structure precedes the first packet.
    [[nodiscard]] std::int64_t lowestFilePosition() const;

private:
    std::vector<StreamPacketTrack> tracks_;
};

/// Parses the `packets` array of an ffprobe run and builds an index.
/// `timebaseFor` resolves a stream index to its timebase; when it returns an
/// invalid rational the packet cannot be placed in time and is reported.
[[nodiscard]] Result<PacketIndex> buildPacketIndex(
    const class JsonValue& packetsArray, const class ContainerModel& model,
    std::size_t* packetsWithoutTimestamp = nullptr);

} // namespace monosect::core
