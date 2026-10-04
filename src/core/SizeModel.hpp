// MonoSect — the size model.
//
// A part's file size is (bytes of the copied packets) + (container structure).
// The packet bytes are known exactly from the source index *before* anything is
// written; the container structure is not, so it is measured and then used
// conservatively.  This is what makes "never exceed the limit" possible without
// promising a precision that cannot be delivered.
#pragma once

#include "core/Bytes.hpp"
#include "core/Rational.hpp"

#include <optional>
#include <string>
#include <vector>

namespace monosect::core {

/// One measured part: what was predicted, what was copied, what landed on disk.
struct SizeObservation {
    ByteCount predictedPayloadBytes = 0;
    ByteCount actualPayloadBytes = 0;     // exact copied bytes, re-indexed from the written part
    ByteCount actualFileBytes = 0;        // the real file size
    Rational duration;                    // measured duration of the part
    std::size_t packetCount = 0;
    std::size_t keyframeCount = 0;

    [[nodiscard]] ByteCount observedOverhead() const {
        return actualFileBytes > actualPayloadBytes ? actualFileBytes - actualPayloadBytes : 0;
    }
};

class SizeModel {
public:
    /// Seeds the model with the information that is known before writing:
    /// how large the container structure of the *source* is (headers, indexes,
    /// attachments, metadata).  `nonPacketSourceBytes` is
    /// sourceFileSize - sum(all packet sizes).
    void seedFromSource(std::string_view formatNames, ByteCount sourceFileBytes, ByteCount sourcePayloadBytes);

    void observe(const SizeObservation& observation);

    /// The overhead allowance that will be added to a predicted payload.  Uses the
    /// largest overhead measured so far (never the average) and a format-specific
    /// default before the first measurement.
    [[nodiscard]] ByteCount overheadAllowance(ByteCount predictedPayloadBytes) const;

    /// Predicted file size for a part whose copied packets amount to `payload`.
    [[nodiscard]] ByteCount predictFileBytes(ByteCount payloadBytes) const {
        return saturatingAdd(payloadBytes, overheadAllowance(payloadBytes));
    }

    [[nodiscard]] bool calibrated() const noexcept { return !observations_.empty(); }
    [[nodiscard]] const std::vector<SizeObservation>& observations() const noexcept { return observations_; }
    [[nodiscard]] std::size_t observationCount() const noexcept { return observations_.size(); }

    /// Explanation of where the current allowance comes from - shown in the plan
    /// and in the UI, because "estimated" and "measured" are not the same thing.
    [[nodiscard]] std::string explanation() const;

    /// Serialised form for the job database.
    [[nodiscard]] std::string toJson() const;
    static SizeModel fromJson(const std::string& text);

    /// The largest payload that can be copied while staying inside `limitBytes`.
    [[nodiscard]] ByteCount maxPayloadForLimit(ByteCount limitBytes) const;

private:
    [[nodiscard]] ByteCount defaultOverhead(ByteCount predictedPayloadBytes) const;

    std::string formatNames_;
    ByteCount sourceFileBytes_ = 0;
    ByteCount sourcePayloadBytes_ = 0;
    ByteCount largestObservedOverhead_ = -1;   // -1 = nothing measured yet
    ByteCount largestObservedPayload_ = 0;
    std::vector<SizeObservation> observations_;
    bool matroska_ = false;
    bool isoBaseMedia_ = false;
    bool transportStream_ = false;
};

} // namespace monosect::core
