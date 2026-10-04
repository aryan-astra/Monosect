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
//   * GPU name: DXGI adapter enumeration, so every vendor is covered - the earlier
//     NVML-only path reported "no GPU" on an Intel machine that plainly had one.
//   * GPU percentage: the "\\GPU Engine(*)\\Utilization Percentage" performance
//     counters Windows publishes for all vendors, which Windows derives from the
//     same vendor interfaces the vendor SDKs wrap.
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
    WindowsPerformanceCounters,
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
    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    /// Samples everything that can be sampled right now.  `mediaBytesWritten` and
    /// `remainingSeconds` come from the job (they are MonoSect's own numbers).
    [[nodiscard]] ResourceSample sample(std::int64_t mediaBytesWritten, double elapsedSeconds,
                                        double cpuSecondsUsedByJob);

    /// Short description of what this machine can actually be measured on: shown
    /// in the resources panel so an "n/a" is explained rather than mysterious.
    [[nodiscard]] const std::string& availabilityNote() const noexcept { return availabilityNote_; }

    /// The display adapters Windows reports, and whether utilization can be read.
    ///
    /// The name is known for every vendor, because it comes from DXGI rather than
    /// from a vendor SDK.  Whether a *percentage* can be read is a separate
    /// question and is answered separately, so an integrated GPU is never shown as
    /// though the machine had no GPU at all.
    ///
    /// One line describing the adapters, e.g.
    /// "Intel(R) UHD Graphics (integrated)".
    [[nodiscard]] std::string gpuDescription() const;

    /// Physical/logical cores, used by the resource policy and shown in the UI.
    [[nodiscard]] int logicalCores() const noexcept { return logicalCores_; }
    [[nodiscard]] int physicalCores() const noexcept { return physicalCores_; }

private:
    [[nodiscard]] MeasuredValue cpuPercent();
    [[nodiscard]] MeasuredValue memoryPercent();
    [[nodiscard]] MeasuredValue gpuPercent();
    [[nodiscard]] MeasuredValue diskBytesPerSecond(bool read);

    /// Opens the "\PhysicalDisk" counters, once.  Members rather than function-local
    /// statics, so the query is owned by this object, is closed when it dies, and does
    /// not leak a process-global PDH query that the unit tests would all share.
    void initDiskCounters();

    /// Enumerates the display adapters, once.  Whether the attempt happened is
    /// recorded even when it found nothing: the set of adapters in a machine does
    /// not change between two samples, so re-asking is pure cost.
    void discoverGpuAdapters();

    /// Opens the "\GPU Engine" counter set and binds it to the adapters found above.
    void initGpuTelemetry();
    void shutdownGpuTelemetry();

    // --- CPU baseline -----------------------------------------------------------------
    std::uint64_t previousIdle_ = 0;
    std::uint64_t previousBusy_ = 0;
    bool cpuBaseline_ = false;

    // --- disk counters ----------------------------------------------------------------
    // These four used to be a delta-based calculation that PDH's own rate counters
    // superseded; they were declared, never referenced, and left behind.  The query and
    // its two counters replaced them.
    void* diskQuery_ = nullptr;       // PDH_HQUERY
    void* diskReadCounter_ = nullptr; // PDH_HCOUNTER
    void* diskWriteCounter_ = nullptr;
    bool diskTried_ = false;

    // --- GPU telemetry -----------------------------------------------------------------
    struct GpuAdapter {
        std::uint32_t luidHigh = 0;
        std::uint32_t luidLow = 0;
        std::string name;
        // The performance-counter instance name fragment for this adapter, e.g.
        // L"luid_0x00000000_0x00008994".  The counters carry the LUID and nothing
        // else that links them to an adapter, so this is the join key.  Wide because
        // that is what PdhGetFormattedCounterArrayW hands back.
        std::wstring luidToken;
    };

    std::vector<GpuAdapter> gpuAdapters_;
    std::string gpuAdapterSummary_;

    bool gpuAdaptersTried_ = false;
    bool gpuTried_ = false;
    // PDH handles, held as void* so this header needs no SDK include.
    void* gpuQuery_ = nullptr;    // PDH_HQUERY
    void* gpuCounter_ = nullptr;  // PDH_HCOUNTER - the wildcard "\GPU Engine" counter

    int logicalCores_ = 1;
    int physicalCores_ = 1;
    std::string availabilityNote_;
};

} // namespace monosect::core
