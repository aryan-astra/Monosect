// MonoSect — resource metrics that are measurements, not decoration
// (TODO 199 - TODO 207, TODO 284 - TODO 290).
//
// Two rules shape this file:
//
//   * A metric that cannot be measured is reported as unavailable, with the
//     reason.  It is never reported as zero (TODO 206/207): a "0 %" GPU graph on
//     a machine whose GPU cannot be queried looks like "the GPU is idle", which
//     is a claim MonoSect has no evidence for.
//   * Every number carries where it came from, because the graphs are read by
//     people trying to decide whether the machine is the bottleneck.
//
// Platform notes (Windows is the product):
//   * CPU: GetSystemTimes gives idle/kernel/user deltas in 100 ns units; the
//     physical core count comes from GetLogicalProcessorInformationEx.
//   * Memory: GlobalMemoryStatusEx.
//   * GPU percentage: NVML (nvmlDeviceGetUtilizationRates) for NVIDIA; the AMD
//     and Intel equivalents are not implemented, so they report unavailable
//     rather than a number that would be invented.
//   * Disk: system-wide read/write bytes per second from PDH
//     ("\\PhysicalDisk(_Total)\\Disk Read Bytes/sec"), which needs no elevation.
//   * Media throughput is measured by MonoSect itself (bytes of published media
//     per second) - it is the one number that is exact.
#pragma once

#include "ui/UiModel.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace monosect::core {

/// Identifies where a measurement comes from, so a graph can say so.
enum class MetricSource {
    OperatingSystemApi,
    Nvml,
    MonoSectItself,
    Unavailable,
};

[[nodiscard]] const char* metricSourceName(MetricSource source) noexcept;

/// One point of a graph.  The UI keeps a bounded history of these.
struct MetricSample {
    double secondsSinceStart = 0.0;
    ResourceSample resources;
};

class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    /// Samples everything that can be sampled right now.  `mediaBytesWritten` and
    /// `remainingSeconds` come from the job (they are MonoSect's own numbers).
    [[nodiscard]] ResourceSample sample(std::int64_t mediaBytesWritten, double elapsedSeconds,
                                        double cpuSecondsUsedByJob);

    /// Short description of what this machine can actually be measured on: shown
    /// in the resources panel so an "n/a" is explained rather than mysterious.
    [[nodiscard]] const std::string& availabilityNote() const noexcept { return availabilityNote_; }

    /// Physical/logical cores, used by the resource policy and shown in the UI.
    [[nodiscard]] int logicalCores() const noexcept { return logicalCores_; }
    [[nodiscard]] int physicalCores() const noexcept { return physicalCores_; }

private:
    [[nodiscard]] MeasuredValue cpuPercent();
    [[nodiscard]] MeasuredValue memoryPercent();
    [[nodiscard]] MeasuredValue gpuPercent();
    [[nodiscard]] MeasuredValue diskBytesPerSecond(bool read);

    void initGpuTelemetry();
    void shutdownGpuTelemetry();

    // --- CPU baseline -----------------------------------------------------------------
    std::uint64_t previousIdle_ = 0;
    std::uint64_t previousBusy_ = 0;
    bool cpuBaseline_ = false;

    // --- disk counters ----------------------------------------------------------------
    std::uint64_t previousReadBytes_ = 0;
    std::uint64_t previousWriteBytes_ = 0;
    std::uint64_t previousDiskSampleNanos_ = 0;
    bool diskBaseline_ = false;

    // --- GPU telemetry -----------------------------------------------------------------
    bool gpuReady_ = false;
    void* gpuLibrary_ = nullptr;
    void* gpuDevice_ = nullptr;
    using NvmlInitFn = int (*)();
    using NvmlShutdownFn = int (*)();
    using NvmlHandleFn = int (*)(unsigned int, void**);
    struct NvmlUtilization {
        unsigned int gpu;
        unsigned int memory;
    };
    using NvmlUtilFn = int (*)(void*, NvmlUtilization*);
    NvmlInitFn nvmlInit_ = nullptr;
    NvmlShutdownFn nvmlShutdown_ = nullptr;
    NvmlHandleFn nvmlHandle_ = nullptr;
    NvmlUtilFn nvmlUtil_ = nullptr;
    std::string gpuName_;

    int logicalCores_ = 1;
    int physicalCores_ = 1;
    std::string availabilityNote_;
};

} // namespace monosect::core
