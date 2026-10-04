#include "core/SizeModel.hpp"

#include "core/Json.hpp"
#include "core/ProbeParsing.hpp"

#include <algorithm>
#include <cstdio>

namespace monosect::core {
namespace {

/// Structural cost that every written part pays regardless of its payload:
/// container headers, the index skeleton, program tables.  These are estimates,
/// not safety padding - the safety of the hard limits comes from three other
/// places: `maxPayloadForLimit()` leaves room for this overhead, every written
/// part is measured and re-plans the rest of the job, and the verification gate
/// refuses to publish a part that is over the limit.
ByteCount structuralFloorFor(bool matroska, bool isoBaseMedia, bool transportStream) {
    if (transportStream) {
        // PAT/PMT repetitions, PCR packets and 188-byte packetisation.
        return 32 * kKiB;
    }
    if (matroska) {
        // EBML header, Segment Info, Tracks with codec private data, Cues.
        return 16 * kKiB;
    }
    if (isoBaseMedia) {
        // ftyp, the moov box headers and per-track metadata.  The sample tables
        // themselves grow with the number of samples and are covered by the
        // proportional term below.
        return 8 * kKiB;
    }
    return 32 * kKiB;
}

/// Container overhead that grows with the payload: per-sample/per-packet index
/// entries, cluster headers, packetisation.
long double proportionalFractionFor(bool matroska, bool isoBaseMedia, bool transportStream) {
    if (transportStream) {
        // 4 header bytes per 188-byte packet, plus alignment stuffing.
        return 0.045L;
    }
    if (matroska) {
        // Cluster headers, SimpleBlock headers and cue entries.
        return 0.012L;
    }
    if (isoBaseMedia) {
        // stts/stsz/stco entries: roughly 20 bytes per sample, and a part of a
        // video file has far fewer samples than it has bytes.
        return 0.020L;
    }
    return 0.030L;
}

} // namespace

void SizeModel::seedFromSource(std::string_view formatNames, ByteCount sourceFileBytes,
                               ByteCount sourcePayloadBytes) {
    formatNames_.assign(formatNames);
    sourceFileBytes_ = sourceFileBytes;
    sourcePayloadBytes_ = sourcePayloadBytes;
    matroska_ = isMatroskaFormat(formatNames);
    isoBaseMedia_ = isIsoBaseMediaFormat(formatNames);
    transportStream_ = isTransportStreamFormat(formatNames);
}

void SizeModel::observe(const SizeObservation& observation) {
    observations_.push_back(observation);
    const ByteCount overhead = observation.observedOverhead();
    if (largestObservedOverhead_ < 0 || overhead > largestObservedOverhead_) {
        largestObservedOverhead_ = overhead;
    }
    largestObservedPayload_ = std::max(largestObservedPayload_, observation.actualPayloadBytes);
}

ByteCount SizeModel::defaultOverhead(ByteCount predictedPayloadBytes) const {
    const ByteCount floorBytes = structuralFloorFor(matroska_, isoBaseMedia_, transportStream_);
    // Everything the source spends outside packet payload: headers, sample
    // tables, cues, attachments and cover art.  A copied part carries a subset
    // of the source's samples, so the source's own non-packet bytes are an upper
    // bound for the same structures inside the part.  Deriving the estimate from
    // the file in front of the user is honest in a way that a made-up constant
    // is not, and it is replaced by measured numbers as soon as the first part
    // has been written.
    const ByteCount nonPacketSourceBytes =
        sourceFileBytes_ > sourcePayloadBytes_ ? sourceFileBytes_ - sourcePayloadBytes_ : 0;
    const ByteCount structural = std::max(floorBytes, nonPacketSourceBytes);
    const long double proportional =
        static_cast<long double>(predictedPayloadBytes) *
        proportionalFractionFor(matroska_, isoBaseMedia_, transportStream_);
    const long double total = static_cast<long double>(structural) + proportional;
    if (total > 9.0e18L) {
        return 9'000'000'000'000'000'000LL;
    }
    return saturatingAdd(structural, static_cast<ByteCount>(proportional));
}

ByteCount SizeModel::overheadAllowance(ByteCount predictedPayloadBytes) const {
    const ByteCount fallback = defaultOverhead(predictedPayloadBytes);
    if (largestObservedOverhead_ < 0) {
        return fallback;
    }
    // Take the larger of (what was measured) and (what a scaling heuristic would
    // suggest for this payload size).  Measured overhead can be smaller for a big
    // part than for a small one only by chance, so the maximum is kept.
    ByteCount proportional = 0;
    if (largestObservedPayload_ > 0 && predictedPayloadBytes > largestObservedPayload_) {
        const long double ratio = static_cast<long double>(predictedPayloadBytes) /
                                  static_cast<long double>(largestObservedPayload_);
        const long double scaled = static_cast<long double>(std::max<ByteCount>(largestObservedOverhead_, 0)) *
                                   (0.5L + 0.5L * ratio);
        proportional = static_cast<ByteCount>(std::min<long double>(scaled, 9.0e18L));
    } else {
        proportional = std::max<ByteCount>(largestObservedOverhead_, 0);
    }
    const ByteCount measured = std::max(proportional, std::max<ByteCount>(largestObservedOverhead_, 0));
    return std::max(measured, fallback / 2);
}

ByteCount SizeModel::maxPayloadForLimit(ByteCount limitBytes) const {
    if (limitBytes <= 0) {
        return 0;
    }
    // Solve payload + overhead(payload) <= limit by bisection.  The overhead
    // function is monotone in the payload, so this converges.
    ByteCount low = 0;
    ByteCount high = limitBytes;
    while (low < high) {
        const ByteCount middle = low + (high - low + 1) / 2;
        if (predictFileBytes(middle) <= limitBytes) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return low;
}

std::string SizeModel::explanation() const {
    std::string text;
    if (observations_.empty()) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "no part measured yet; the container overhead is estimated at %s",
                      formatBytesShort(defaultOverhead(0)).c_str());
        text = buffer;
        text += " and every written part is verified before it is published";
        return text;
    }
    text = "measured from " + std::to_string(observations_.size()) + " part(s)";
    text += ", largest container overhead seen: " + formatBytesExact(std::max<ByteCount>(largestObservedOverhead_, 0));
    ByteCount predictedSum = 0;
    ByteCount actualSum = 0;
    for (const auto& observation : observations_) {
        predictedSum += observation.predictedPayloadBytes;
        actualSum += observation.actualPayloadBytes;
    }
    if (predictedSum > 0) {
        const double errorPercent =
            100.0 * static_cast<double>(predictedSum - actualSum) / static_cast<double>(predictedSum);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "; payload prediction was off by %.3f%%", errorPercent);
        text += buffer;
    }
    return text;
}

std::string SizeModel::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root.set("format_names", JsonValue::makeString(formatNames_));
    root.set("source_file_bytes", JsonValue::makeInt(sourceFileBytes_));
    root.set("source_payload_bytes", JsonValue::makeInt(sourcePayloadBytes_));
    root.set("largest_observed_overhead", JsonValue::makeInt(std::max<ByteCount>(largestObservedOverhead_, -1)));
    root.set("largest_observed_payload", JsonValue::makeInt(largestObservedPayload_));
    JsonValue list = JsonValue::makeArray();
    for (const auto& observation : observations_) {
        JsonValue item = JsonValue::makeObject();
        item.set("predicted_payload", JsonValue::makeInt(observation.predictedPayloadBytes));
        item.set("actual_payload", JsonValue::makeInt(observation.actualPayloadBytes));
        item.set("actual_file", JsonValue::makeInt(observation.actualFileBytes));
        item.set("duration_num", JsonValue::makeInt(observation.duration.numerator));
        item.set("duration_den", JsonValue::makeInt(observation.duration.denominator));
        item.set("packets", JsonValue::makeInt(static_cast<std::int64_t>(observation.packetCount)));
        item.set("keyframes", JsonValue::makeInt(static_cast<std::int64_t>(observation.keyframeCount)));
        list.push(std::move(item));
    }
    root.set("observations", std::move(list));
    return root.dump();
}

SizeModel SizeModel::fromJson(const std::string& text) {
    SizeModel model;
    auto parsed = parseJson(text);
    if (!parsed.has_value() || !parsed.value().isObject()) {
        return model;
    }
    const JsonValue& root = parsed.value();
    model.seedFromSource(root.stringOr("format_names"), 
                         parseFfprobeInteger(root, "source_file_bytes").value_or(0),
                         parseFfprobeInteger(root, "source_payload_bytes").value_or(0));
    if (auto value = parseFfprobeInteger(root, "largest_observed_overhead"); value.has_value()) {
        model.largestObservedOverhead_ = *value;
    }
    if (auto value = parseFfprobeInteger(root, "largest_observed_payload"); value.has_value()) {
        model.largestObservedPayload_ = *value;
    }
    if (const JsonValue* list = root.find("observations"); list != nullptr && list->isArray()) {
        for (const JsonValue& item : list->items()) {
            if (!item.isObject()) {
                continue;
            }
            SizeObservation observation;
            observation.predictedPayloadBytes = parseFfprobeInteger(item, "predicted_payload").value_or(0);
            observation.actualPayloadBytes = parseFfprobeInteger(item, "actual_payload").value_or(0);
            observation.actualFileBytes = parseFfprobeInteger(item, "actual_file").value_or(0);
            observation.duration = Rational{parseFfprobeInteger(item, "duration_num").value_or(0),
                                            parseFfprobeInteger(item, "duration_den").value_or(1)};
            observation.packetCount = static_cast<std::size_t>(parseFfprobeInteger(item, "packets").value_or(0));
            observation.keyframeCount = static_cast<std::size_t>(parseFfprobeInteger(item, "keyframes").value_or(0));
            model.observations_.push_back(observation);
        }
    }
    return model;
}

} // namespace monosect::core
