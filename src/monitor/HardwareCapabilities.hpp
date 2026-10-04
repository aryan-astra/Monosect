// MonoSect — hardware encoder capability probing (TODO 187 - TODO 192).
//
// The old detector answered a different question than the one the product needs.
// It asked FFmpeg which encoders *exist in this build* and reported them as
// available hardware.  On a machine with no NVIDIA card at all that reported
// `h264_nvenc`, `hevc_nvenc`, `h264_qsv` and `h264_amf` as usable, because a
// static FFmpeg build contains all of them.  Selecting one then failed at run
// time, and - worse for the product's promise - it silently invalidated the
// "copied bit for bit" guarantee, because the user had asked for a copy and got
// an encode.
//
// TODO 191 states the rule this file implements: *do not assume encoder
// availability means encoder usability*.  Availability is what
// `ffmpeg -encoders` prints.  Usability is whether a real one-frame encode
// succeeds on this machine, right now.  Only the second one may be offered.
//
// The probe therefore runs two stages:
//
//   1. `ffmpeg -encoders` - cheap, one process, tells us which vendor encoders
//      the build even knows about.  A candidate that is not listed is not probed
//      at all: it cannot work.
//   2. one synthetic one-frame encode per listed candidate.  The source is
//      FFmpeg's own `color` lavfi source, so no temporary file is created and
//      nothing on disk can influence the result.
//
// A probe that hangs (a driver that never returns) is bounded by a wall-clock
// deadline that cancels the process, and the encoder is reported as "not usable:
// the probe did not finish".  A timeout is a result, not a silence.
//
// Research basis (docs/RESEARCH.md section 2): FFmpeg's encoder documentation
// states that an encoder's presence in `-encoders` says nothing about whether the
// corresponding device or driver can run it; the vendor APIs (NVML, Intel Media
// SDK, AMF) are the only authority, and their own failure modes are exactly what
// a live encode surfaces.
#pragma once

#include "core/ResourcePolicy.hpp"
#include "engine/ProcessRunner.hpp"

#include <string>
#include <vector>

namespace monosect::core {

/// One hardware encoder, as the machine answered about it.
struct EncoderProbe {
    std::string encoder;      // "h264_nvenc"
    std::string vendor;       // "NVIDIA" | "Intel" | "AMD"
    std::string codecName;    // "h264" | "hevc"
    bool listed = false;      // appears in `ffmpeg -encoders`
    bool usable = false;      // a one-frame encode succeeded on this machine
    std::string detail;       // why it is not usable; always filled in when !usable
    std::string description;  // FFmpeg's own one-line description

    [[nodiscard]] bool offersHardwareAcceleration() const noexcept {
        return listed && usable;
    }
};

/// What this machine can actually do, and what it cannot.
struct HardwareCapabilities {
    /// True once `probe()` has run to completion.  False means "not probed yet",
    /// which the UI shows as such - never as "no hardware".
    bool probed = false;
    std::string probeError;        // set when the probe itself could not run
    std::vector<EncoderProbe> encoders;

    /// Vendors for which a usable encoder was found, in probe order.
    std::vector<std::string> usableVendors;

    /// Whether GPU utilization telemetry can be read (NVML on Windows).
    bool telemetryAvailable = false;
    std::string telemetryDetail;

    /// Wall-clock seconds the last probe took.
    double probeSeconds = 0.0;

    /// The usable encoder MonoSect would use for `codecName`, preferring the
    /// vendors in `preferredOrder`, or an empty string when none is usable.
    [[nodiscard]] std::string usableEncoderFor(const std::string& codecName) const;

    /// The first usable encoder for `codecName`, ignoring preference.
    [[nodiscard]] std::string anyUsableEncoderFor(const std::string& codecName) const;

    /// The view core/ResourcePolicy derives its policy from.
    [[nodiscard]] GpuCapability toGpuCapability(bool userEnabledGpu, bool jobWillEncode) const;

    /// One-paragraph summary for the resources panel.  It always says what was
    /// checked, so a "no" is never ambiguous between "absent" and "untested".
    [[nodiscard]] std::string describe() const;
};

/// Runs the probe.  Not thread-safe: one instance, one probe, one result.
class HardwareCapabilityProbe {
public:
    HardwareCapabilityProbe(IProcessRunner& runner, std::string ffmpegPath,
                            double perEncoderTimeoutSeconds = 8.0);

    /// Runs `ffmpeg -encoders` and then one encode per listed hardware encoder.
    /// Never throws and never blocks indefinitely.
    [[nodiscard]] HardwareCapabilities probe();

    /// Vendors MonoSect knows how to talk about, in the order it prefers them
    /// when more than one can encode the same codec.
    [[nodiscard]] static const std::vector<std::string>& vendorPreferenceOrder();

private:
    [[nodiscard]] std::vector<EncoderProbe> listEncoders();
    /// Encodes a single black frame with `encoder`.  Returns false and fills
    /// `detail` when the encoder refuses, when it is not listed, or when the
    /// probe did not finish inside the deadline.
    [[nodiscard]] bool testEncoder(const std::string& encoder, std::string& detail);

    IProcessRunner& runner_;
    std::string ffmpegPath_;
    double perEncoderTimeoutSeconds_;
};

} // namespace monosect::core
