#include "monitor/HardwareCapabilities.hpp"

#include "core/Sha256.hpp"
#include "engine/ProcessRunners.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace monosect::core {
namespace {

/// The hardware encoders MonoSect knows how to probe, with the vendor and the
/// codec each one produces.  A build that lists anything outside this table is
/// still described in the log; it is simply not offered, because MonoSect has no
/// mapping from it to a preservation decision.
struct EncoderCandidate {
    const char* encoder;
    const char* vendor;
    const char* codec;
};

constexpr EncoderCandidate kCandidates[] = {
    {"h264_nvenc", "NVIDIA", "h264"},
    {"hevc_nvenc", "NVIDIA", "hevc"},
    {"h264_qsv", "Intel", "h264"},
    {"hevc_qsv", "Intel", "hevc"},
    {"h264_amf", "AMD", "h264"},
    {"hevc_amf", "AMD", "hevc"},
    {"h264_videotoolbox", "Apple", "h264"},
    {"hevc_videotoolbox", "Apple", "hevc"},
};

std::string vendorOf(const std::string& encoder) {
    for (const EncoderCandidate& candidate : kCandidates) {
        if (encoder == candidate.encoder) {
            return candidate.vendor;
        }
    }
    return {};
}

std::string codecOf(const std::string& encoder) {
    for (const EncoderCandidate& candidate : kCandidates) {
        if (encoder == candidate.encoder) {
            return candidate.codec;
        }
    }
    return {};
}

/// True when FFmpeg's `-encoders` flag column looks like one.  The column is six
/// characters drawn from `-.AVSDFTXBOLmisl8I`; anything else is a banner or a
/// blank line and must not be mistaken for a row.
bool looksLikeFlagColumn(const std::string& column) {
    if (column.size() != 6) {
        return false;
    }
    static const std::string kAllowed = "-.AVSDFTXBOLmisl8I";
    for (const char c : column) {
        if (kAllowed.find(c) == std::string::npos) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> splitWhitespace(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        out.push_back(token);
    }
    return out;
}

/// True when nvml.dll can be loaded, which is the only telemetry interface
/// MonoSect implements.  The AMD and Intel equivalents are deliberately absent
/// (see the class comment): reporting a guessed number for a GPU MonoSect cannot
/// measure would be a false claim, so those machines get "n/a" with a reason.
bool nvmlTelemetryAvailable(std::string& detail) {
#if defined(_WIN32)
    HMODULE library = ::LoadLibraryW(L"nvml.dll");
    if (library == nullptr) {
        detail = "NVML is not present, which means no NVIDIA driver is installed on this machine";
        return false;
    }
    const bool hasUtilization = ::GetProcAddress(library, "nvmlDeviceGetUtilizationRates") != nullptr;
    const bool hasInit = ::GetProcAddress(library, "nvmlInit_v2") != nullptr;
    ::FreeLibrary(library);
    if (!hasInit || !hasUtilization) {
        detail = "nvml.dll is present but does not expose nvmlInit_v2/nvmlDeviceGetUtilizationRates";
        return false;
    }
    detail = "NVIDIA NVML";
    return true;
#else
    detail = "GPU telemetry is only implemented for NVIDIA GPUs on Windows";
    return false;
#endif
}

} // namespace

const std::vector<std::string>& HardwareCapabilityProbe::vendorPreferenceOrder() {
    // NVENC first: it is the most widely available and the most predictable in
    // its rate control.  The order only decides which encoder is picked when a
    // machine has more than one usable; the user is told which one was chosen.
    static const std::vector<std::string> order{"NVIDIA", "Intel", "AMD", "Apple"};
    return order;
}

HardwareCapabilityProbe::HardwareCapabilityProbe(IProcessRunner& runner, std::string ffmpegPath,
                                                 double perEncoderTimeoutSeconds)
    : runner_(runner), ffmpegPath_(std::move(ffmpegPath)),
      perEncoderTimeoutSeconds_(perEncoderTimeoutSeconds > 0.5 ? perEncoderTimeoutSeconds : 8.0) {}

std::vector<EncoderProbe> HardwareCapabilityProbe::listEncoders() {
    std::vector<EncoderProbe> out;
    ProcessSpec spec;
    spec.program = ffmpegPath_;
    spec.arguments = {"-hide_banner", "-nostdin", "-loglevel", "error", "-encoders"};
    spec.captureStdout = true;
    const ProcessResult result = runner_.run(spec, ProcessCallbacks{});
    if (!result.launched) {
        return out;
    }
    for (const std::string& line : splitLines(result.stdoutText)) {
        // " V....D h264_nvenc    NVIDIA NVENC H.264 encoder"
        const std::vector<std::string> columns = splitWhitespace(line);
        if (columns.size() < 2 || !looksLikeFlagColumn(columns[0])) {
            continue;
        }
        const std::string& name = columns[1];
        // Video encoders only: MonoSect's re-encode paths are video and audio,
        // and an audio hardware encoder would have to be probed separately.
        if (columns[0].find('V') == std::string::npos) {
            continue;
        }
        bool interesting = false;
        for (const EncoderCandidate& candidate : kCandidates) {
            if (name == candidate.encoder) {
                interesting = true;
                break;
            }
        }
        if (!interesting) {
            continue;
        }
        EncoderProbe probe;
        probe.encoder = name;
        probe.vendor = vendorOf(name);
        probe.codecName = codecOf(name);
        probe.listed = true;
        std::istringstream description(line);
        std::string flags;
        std::string ignored;
        description >> flags >> ignored;
        std::getline(description, probe.description);
        // Trim the leading whitespace the stream left behind.
        const std::size_t first = probe.description.find_first_not_of(" \t");
        probe.description = first == std::string::npos ? std::string() : probe.description.substr(first);
        out.push_back(std::move(probe));
    }
    return out;
}

bool HardwareCapabilityProbe::testEncoder(const std::string& encoder, std::string& detail) {
    // A one-frame encode of FFmpeg's own synthetic source, written to the null
    // muxer: nothing is written to disk, so the result depends only on the
    // encoder and the machine.
    //
    // `-pix_fmt yuv420p` is passed on the first attempt because most vendor
    // encoders reject an unspecified pixel format; if that is what failed, a
    // second attempt without it distinguishes "this encoder cannot run" from
    // "this encoder needs a pixel format MonoSect did not guess", which are
    // different answers for the user.
    struct Attempt {
        bool withPixelFormat;
    };
    const Attempt attempts[] = {{true}, {false}};

    std::string lastDetail;
    for (const Attempt& attempt : attempts) {
        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = {"-hide_banner", "-nostdin", "-loglevel", "error",
                          "-f", "lavfi", "-i", "color=c=black:s=128x128:r=25",
                          "-frames:v", "1"};
        if (attempt.withPixelFormat) {
            spec.arguments.push_back("-pix_fmt");
            spec.arguments.push_back("yuv420p");
        }
        spec.arguments.push_back("-c:v");
        spec.arguments.push_back(encoder);
        spec.arguments.push_back("-f");
        spec.arguments.push_back("null");
        spec.arguments.push_back("-");
        spec.captureStderr = true;

        // A bounded probe: a wedged driver must not freeze the application.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<int>(perEncoderTimeoutSeconds_ * 1000.0));
        auto expired = std::make_shared<std::atomic<bool>>(false);
        auto clock = std::make_shared<std::chrono::steady_clock::time_point>(deadline);
        ProcessCallbacks callbacks;
        callbacks.shouldCancel = [expired, clock]() {
            if (std::chrono::steady_clock::now() >= *clock) {
                expired->store(true);
                return true;
            }
            return false;
        };

        const ProcessResult result = runner_.run(spec, callbacks);
        if (result.canceled || expired->load()) {
            detail = "the probe did not finish within " + std::to_string(static_cast<int>(perEncoderTimeoutSeconds_)) +
                     " s, so the encoder is treated as unusable";
            return false;
        }
        if (result.launched && result.exitCode == 0) {
            detail.clear();
            return true;
        }
        if (!result.launched) {
            detail = "FFmpeg could not be started for the probe: " + result.launchError;
            return false;
        }
        lastDetail = joinForDisplay(result.stderrTail, 400);
    }
    if (lastDetail.empty()) {
        detail = "the one-frame test encode failed without a message";
    } else {
        detail = "the one-frame test encode failed: " + lastDetail;
    }
    return false;
}

HardwareCapabilities HardwareCapabilityProbe::probe() {
    HardwareCapabilities capabilities;
    const auto started = std::chrono::steady_clock::now();

    capabilities.encoders = listEncoders();
    if (capabilities.encoders.empty()) {
        // No candidate encoder is even present in this FFmpeg build, which is a
        // real answer (not a failed probe) - but it must be distinguishable from
        // "FFmpeg could not be run", so the reason is recorded either way.
        ProcessSpec spec;
        spec.program = ffmpegPath_;
        spec.arguments = {"-hide_banner", "-nostdin", "-loglevel", "error", "-encoders"};
        const ProcessResult result = runner_.run(spec, ProcessCallbacks{});
        if (!result.launched) {
            capabilities.probeError = "FFmpeg could not be started, so no encoder could be checked: " +
                                      result.launchError;
        } else {
            capabilities.probeError =
                "This FFmpeg build lists none of the hardware encoders MonoSect knows how to test.";
        }
    }

    for (EncoderProbe& probe : capabilities.encoders) {
        std::string detail;
        probe.usable = testEncoder(probe.encoder, detail);
        probe.detail = probe.usable ? "a one-frame test encode succeeded on this machine" : detail;
        if (probe.usable &&
            std::find(capabilities.usableVendors.begin(), capabilities.usableVendors.end(), probe.vendor) ==
                capabilities.usableVendors.end()) {
            capabilities.usableVendors.push_back(probe.vendor);
        }
    }

    std::string telemetryDetail;
    capabilities.telemetryAvailable = nvmlTelemetryAvailable(telemetryDetail);
    capabilities.telemetryDetail = telemetryDetail;
    capabilities.probed = true;
    capabilities.probeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return capabilities;
}

std::string HardwareCapabilities::anyUsableEncoderFor(const std::string& codecName) const {
    for (const EncoderProbe& probe : encoders) {
        if (probe.usable && probe.codecName == codecName) {
            return probe.encoder;
        }
    }
    return {};
}

std::string HardwareCapabilities::usableEncoderFor(const std::string& codecName) const {
    for (const std::string& vendor : HardwareCapabilityProbe::vendorPreferenceOrder()) {
        for (const EncoderProbe& probe : encoders) {
            if (probe.usable && probe.codecName == codecName && probe.vendor == vendor) {
                return probe.encoder;
            }
        }
    }
    return anyUsableEncoderFor(codecName);
}

GpuCapability HardwareCapabilities::toGpuCapability(bool userEnabledGpu, bool jobWillEncode) const {
    GpuCapability gpu;
    gpu.telemetryAvailable = telemetryAvailable;
    if (!probed) {
        gpu.vendor = "none";
        gpu.name = "not probed yet";
        gpu.probeDetail = "the hardware encoder probe has not run on this machine yet";
        return gpu;
    }
    if (!probeError.empty() && encoders.empty()) {
        gpu.vendor = "none";
        gpu.name = "no usable hardware encoder";
        gpu.probeDetail = probeError;
        return gpu;
    }
    // `encoderPresent` means "listed", `encoderUsable` means "a real encode
    // worked".  ResourcePolicy only picks a GPU encoder when both are true, so a
    // listed-but-broken encoder can never be selected.
    bool anyListed = false;
    for (const EncoderProbe& probe : encoders) {
        anyListed = anyListed || probe.listed;
        if (!probe.usable && !probe.detail.empty() &&
            gpu.probeDetail.find(probe.encoder) == std::string::npos) {
            gpu.probeDetail += (gpu.probeDetail.empty() ? "" : "; ");
            gpu.probeDetail += probe.encoder + ": " + probe.detail;
        }
    }
    gpu.encoderPresent = anyListed;
    const std::string encoder = userEnabledGpu && jobWillEncode ? usableEncoderFor("h264") : std::string{};
    if (!encoder.empty()) {
        gpu.encoderUsable = true;
        gpu.usableEncoder = encoder;
        for (const EncoderProbe& probe : encoders) {
            if (probe.encoder == encoder) {
                gpu.vendor = probe.vendor;
                gpu.name = probe.description;
                break;
            }
        }
    } else if (anyListed && jobWillEncode) {
        gpu.vendor = usableVendors.empty() ? "none" : usableVendors.front();
        gpu.name = "hardware encoders are listed but none of them produced a frame";
    } else {
        gpu.vendor = "none";
        gpu.name = "no hardware encoder is needed by this job (it copies packets)";
    }
    return gpu;
}

std::string HardwareCapabilities::describe() const {
    std::ostringstream text;
    if (!probed) {
        text << "The hardware encoder probe has not run yet, so MonoSect cannot say what this machine can "
                "accelerate. Until it has, the software encoders are used.";
        return text.str();
    }
    if (!probeError.empty()) {
        text << probeError << " ";
    }
    if (encoders.empty()) {
        text << "No hardware video encoder was found. MonoSect will use the software encoders when it has to "
                "re-encode at all.";
        return text.str();
    }
    int usable = 0;
    for (const EncoderProbe& probe : encoders) {
        if (probe.usable) {
            ++usable;
        }
    }
    text << "Checked " << encoders.size() << " hardware encoder(s) with a real one-frame test encode: " << usable
         << " usable, " << (encoders.size() - static_cast<std::size_t>(usable)) << " refused. ";
    for (const EncoderProbe& probe : encoders) {
        text << (probe.usable ? "  " : "  ") << probe.encoder << ": " << (probe.usable ? "usable" : "not usable")
             << (probe.usable ? std::string() : " (" + probe.detail + ")") << "\n";
    }
    text << "  GPU utilization telemetry: " << (telemetryAvailable ? telemetryDetail : "not available - " +
                                               telemetryDetail)
         << ".";
    return text.str();
}

} // namespace monosect::core
