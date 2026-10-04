// MonoSect — resource policy (TODO 182 - TODO 193, TODO 194 - TODO 198).
//
// "Use 100 % CPU" is not the same as "finish sooner": FFmpeg saturates memory
// bandwidth long before it saturates cores, and a job that is disk bound gets
// slower when it competes with itself. The policy below therefore separates the
// *user's* choice from the *derived* settings, and the UI shows what was derived
// and why (TODO 193).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace monosect::core {

enum class ResourceMode {
    Conservative,   // leave the machine usable: low thread counts, normal priority
    Balanced,       // default: use most cores, below normal priority
    Maximum,        // use everything that can help, above normal priority
    Custom,         // explicit numbers from the user
};

[[nodiscard]] const char* resourceModeName(ResourceMode mode) noexcept;
[[nodiscard]] std::string resourceModeDescription(ResourceMode mode);

struct CpuTopology {
    int logicalCores = 1;
    int physicalCores = 1;
    bool hybrid = false;             // P/E cores (Intel 12th gen and later)
    int performanceCores = 0;
    int efficiencyCores = 0;

    // Derived from the Windows API documentation: GetActiveProcessorCount +
    // GetLogicalProcessorInformationEx(RelationProcessorCore) give physical cores;
    // the hybrid split comes from the EfficiencyClass field of
    // SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX.
    static CpuTopology detect();
    [[nodiscard]] std::string describe() const;
};

struct GpuCapability {
    std::string vendor;                 // "NVIDIA" | "AMD" | "Intel" | "none"
    std::string name;
    bool encoderPresent = false;        // the encoder exists in `ffmpeg -encoders`
    bool encoderUsable = false;         // a 1 frame probe actually succeeded (TODO 191/192)
    std::string usableEncoder;          // "h264_nvenc", "hevc_qsv", "h264_amf", ...
    std::string probeDetail;            // why it is not usable, when it is not
    bool telemetryAvailable = false;    // NVML / PDH counters readable
};

struct ResourcePolicy {
    ResourceMode mode = ResourceMode::Balanced;
    int encoderThreads = 0;             // encoder workers
    int filterThreads = 0;              // filter graph workers
    int concurrentSegments = 1;         // always 1 by default (TODO 196)
    bool useGpuEncoder = false;
    std::string gpuEncoder;
    bool useHardwareDecoding = false;
    int priorityDelta = 0;
    int64_t ffmpegNiceLevel = 0;        // Windows process priority class offset
    std::string rationale;              // shown next to the resources panel
};

struct ResourcePolicyInput {
    ResourceMode mode = ResourceMode::Balanced;
    CpuTopology topology;
    GpuCapability gpu;
    bool userEnabledGpu = true;
    bool userEnabledHardwareDecoding = true;
    // When the output format is MP4/TS the copy path needs no encoder at all, so
    // GPU settings only matter for smart-cut boundaries.
    bool jobWillEncode = false;
    // Disk throughput of the destination, when known (bytes/second, 0 = unknown).
    int64_t destinationThroughputBytesPerSecond = 0;
};

[[nodiscard]] ResourcePolicy deriveResourcePolicy(const ResourcePolicyInput& input);

// TODO 197: whether more than one FFmpeg process inside a single job helps. The
// answer depends on the storage device: on a single spinning disk or a saturated
// SATA SSD, parallel segment writes reduce throughput. The function returns the
// decision plus the reason so the UI can display it instead of guessing.
struct ConcurrencyDecision {
    int processes = 1;
    std::string reason;
};

[[nodiscard]] ConcurrencyDecision decideSegmentConcurrency(const ResourcePolicyInput& input,
                                                           bool sourceAndDestinationOnSameVolume);

} // namespace monosect::core
