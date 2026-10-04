#include "monitor/SystemMonitor.hpp"

#include <cstdio>
#include <iterator>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
// IDXGIFactory1 / IDXGIAdapter1 / DXGI_ADAPTER_DESC1.  DXGI is how Windows itself
// names every display adapter from every vendor, which is what replaced the
// NVML-only enumeration.
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#else
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <thread>
#endif

namespace monosect::core {
namespace {

/// One number, rounded where rounding is honest and left alone where it is not.
double ratio(double numerator, double denominator) {
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

} // namespace

const char* metricSourceName(MetricSource source) noexcept {
    switch (source) {
        case MetricSource::OperatingSystemApi: return "operating system counters";
        case MetricSource::WindowsPerformanceCounters: return "Windows performance counters";
        case MetricSource::MonoSectItself: return "measured by MonoSect";
        case MetricSource::Unavailable: return "not available on this machine";
    }
    return "unknown";
}

SystemMonitor::SystemMonitor() {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    logicalCores_ = static_cast<int>(info.dwNumberOfProcessors);
    if (logicalCores_ < 1) {
        logicalCores_ = 1;
    }
    // Physical cores: count the processor cores the OS reports.  On hybrid CPUs
    // (Intel 12th gen and later) the efficiency class distinguishes P and E
    // cores; both are counted here, and ResourcePolicy decides how to use them.
    physicalCores_ = 0;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length > 0) {
        std::vector<unsigned char> buffer(length);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                             reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                                                 buffer.data()),
                                             &length)) {
            std::size_t offset = 0;
            while (offset < length) {
                const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    buffer.data() + offset);
                ++physicalCores_;
                // Defensive only: the OS contract guarantees Size >= sizeof(...), but a zero
    // would make this loop spin forever on a kernel that returned a record it had
    // also said had no size.
    if (entry->Size == 0) {
        break;
    }
    offset += entry->Size;
            }
        }
    }
    if (physicalCores_ < 1) {
        physicalCores_ = logicalCores_;
    }
#else
    unsigned int cores = std::thread::hardware_concurrency();
    logicalCores_ = cores == 0 ? 1 : static_cast<int>(cores);
    physicalCores_ = logicalCores_;
#endif
    availabilityNote_ = "CPU and memory from the operating system";
    // The adapters in a machine do not change while it is running, so they are
    // enumerated once here rather than on the first sample.  It matters for more than
    // tidiness: the UI asks for the adapter *name* the moment the panel is drawn, and
    // deferring this to the first sample made a machine with a GPU announce "no display
    // adapter" for the first second.
    discoverGpuAdapters();
}

SystemMonitor::~SystemMonitor() {
    shutdownGpuTelemetry();
#if defined(_WIN32)
    if (diskQuery_ != nullptr) {
        PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(diskQuery_));
    }
#endif
    diskQuery_ = nullptr;
}

MeasuredValue SystemMonitor::cpuPercent() {
#if defined(_WIN32)
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return MeasuredValue::unknown(std::string("GetSystemTimes failed (error ") +
                                      std::to_string(GetLastError()) + ")");
    }
    const auto toUint64 = [](const FILETIME& time) {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return static_cast<std::uint64_t>(value.QuadPart);
    };
    const std::uint64_t idleTicks = toUint64(idle);
    // Kernel time includes idle time; busy is kernel + user - idle.
    const std::uint64_t busyTicks = toUint64(kernel) + toUint64(user) - idleTicks;
    if (!cpuBaseline_) {
        previousIdle_ = idleTicks;
        previousBusy_ = busyTicks;
        cpuBaseline_ = true;
        return MeasuredValue::unknown("the first sample establishes the baseline");
    }
    const std::uint64_t idleDelta = idleTicks >= previousIdle_ ? idleTicks - previousIdle_ : 0;
    const std::uint64_t busyDelta = busyTicks >= previousBusy_ ? busyTicks - previousBusy_ : 0;
    previousIdle_ = idleTicks;
    previousBusy_ = busyTicks;
    if (idleDelta + busyDelta == 0) {
        return MeasuredValue::unknown("no CPU time elapsed between the two samples");
    }
    return MeasuredValue::of(100.0 * ratio(static_cast<double>(busyDelta),
                                           static_cast<double>(idleDelta + busyDelta)));
#elif defined(__linux__)
    std::ifstream stat("/proc/stat");
    std::string line;
    if (!std::getline(stat, line)) {
        return MeasuredValue::unknown(" /proc/stat could not be read");
    }
    std::istringstream stream(line);
    std::string label;
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    stream >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
    const std::uint64_t idleTicks = idle + iowait;
    const std::uint64_t busyTicks = user + nice + system + irq + softirq;
    if (!cpuBaseline_) {
        previousIdle_ = idleTicks;
        previousBusy_ = busyTicks;
        cpuBaseline_ = true;
        return MeasuredValue::unknown("the first sample establishes the baseline");
    }
    const std::uint64_t idleDelta = idleTicks >= previousIdle_ ? idleTicks - previousIdle_ : 0;
    const std::uint64_t busyDelta = busyTicks >= previousBusy_ ? busyTicks - previousBusy_ : 0;
    previousIdle_ = idleTicks;
    previousBusy_ = busyTicks;
    if (idleDelta + busyDelta == 0) {
        return MeasuredValue::unknown("no CPU time elapsed between the two samples");
    }
    return MeasuredValue::of(100.0 * ratio(static_cast<double>(busyDelta),
                                           static_cast<double>(idleDelta + busyDelta)));
#else
    return MeasuredValue::unknown("CPU counters are not implemented on this platform");
#endif
}

MeasuredValue SystemMonitor::memoryPercent() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return MeasuredValue::unknown(std::string("GlobalMemoryStatusEx failed (error ") +
                                      std::to_string(GetLastError()) + ")");
    }
    return MeasuredValue::of(static_cast<double>(status.dwMemoryLoad));
#elif defined(__linux__)
    std::ifstream info("/proc/meminfo");
    std::string line;
    double total = 0.0;
    double available = 0.0;
    while (std::getline(info, line)) {
        std::istringstream stream(line);
        std::string key;
        double value = 0.0;
        std::string unit;
        stream >> key >> value >> unit;
        if (key == "MemTotal:") {
            total = value;
        } else if (key == "MemAvailable:") {
            available = value;
        }
    }
    if (total <= 0.0) {
        return MeasuredValue::unknown("/proc/meminfo did not report MemTotal");
    }
    return MeasuredValue::of(100.0 * ratio(total - available, total));
#else
    return MeasuredValue::unknown("memory counters are not implemented on this platform");
#endif
}

void SystemMonitor::discoverGpuAdapters() {
    // Guarded on "tried", not on a handle: the adapters in a machine do not change
    // between two samples, so this is done once and the answer kept.  The previous
    // NVML-based version guarded on the library handle, which stays null on every
    // failure path, so on any machine without an NVIDIA card the search - and its
    // sentence of explanation - repeated on every sample.
    if (gpuAdaptersTried_) {
        return;
    }
    gpuAdaptersTried_ = true;

#if defined(_WIN32)
    // DXGI, not a vendor SDK.  The old code asked NVML for a device, which is a
    // reasonable thing to do on an NVIDIA machine and reports nothing whatsoever on
    // every other vendor - so a laptop with an Intel integrated GPU was shown as
    // having no GPU at all.  DXGI is the interface Windows itself uses and names
    // every adapter from every vendor.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory == nullptr) {
        gpuAdapterSummary_ = "DXGI is unavailable, so no adapter could be named";
        return;
    }

    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (adapter == nullptr) {
            continue;
        }
        DXGI_ADAPTER_DESC1 description{};
        const HRESULT described = adapter->GetDesc1(&description);
        adapter->Release();
        if (FAILED(described)) {
            continue;
        }
        // The software fallback adapter and the Microsoft Basic Display Adapter are
        // what DXGI reports for "no real GPU"; listing them would put a fake card in
        // front of the user and is exactly the kind of invented answer this file
        // exists to avoid.
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
            description.VendorId == 0x1414 /* Microsoft */) {
            continue;
        }

        GpuAdapter entry;
        entry.luidHigh = static_cast<std::uint32_t>(description.AdapterLuid.HighPart);
        entry.luidLow = static_cast<std::uint32_t>(description.AdapterLuid.LowPart);
        // No integrated/discrete label, because there is no way to get one that can be
        // trusted: DedicatedVideoMemory is *not* the test.  Measured on an Intel UHD
        // integrated part, DXGI reports 128 MiB of dedicated memory - a reserved BIOS
        // region - and Windows would call the same card "discrete" on that number.
        // IDXGIAdapter3::GetDesc3's GraphicsPreemptionGranularity is the documented
        // discriminator and MinGW's DXGI headers stop at IDXGIAdapter2, so rather than
        // guess in a panel whose whole purpose is not guessing, the panel states the
        // name and nothing it cannot support.

        // DXGI_ADAPTER_DESC1::Description is a WCHAR[128] that the documentation
        // describes as NUL-padded rather than guaranteed NUL-terminated, so the
        // length is bounded explicitly rather than by strlen, and the two-call size
        // query means a name that somehow fills all 128 units still converts rather
        // than degrading to "unnamed adapter".
        {
            constexpr int capacity = 128;
            const int needed = WideCharToMultiByte(CP_UTF8, 0, description.Description, capacity, nullptr, 0,
                                                   nullptr, nullptr);
            if (needed > 0) {
                std::string narrow(static_cast<std::size_t>(needed), '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, description.Description, capacity, narrow.data(),
                                        needed, nullptr, nullptr) > 0) {
                    while (!narrow.empty() && narrow.back() == '\0') {
                        narrow.pop_back();
                    }
                    entry.name = narrow;
                }
            }
        }
        if (entry.name.empty()) {
            entry.name = "unnamed adapter";
        }

        wchar_t token[64]{};
        std::swprintf(token, std::size(token), L"luid_0x%08X_0x%08X", entry.luidHigh, entry.luidLow);
        entry.luidToken = token;

        gpuAdapters_.push_back(std::move(entry));
        if (gpuAdapters_.size() == 8) {
            // A machine with more than eight adapters is not a thing, and the
            // enumeration below is unbounded.
            break;
        }
    }
    factory->Release();

    if (gpuAdapters_.empty()) {
        gpuAdapterSummary_ = "Windows reports no display adapter through DXGI";
        return;
    }

    gpuAdapterSummary_ = gpuDescription();
#else
    gpuAdapterSummary_ = "display adapter enumeration is only implemented on Windows";
#endif
}

std::string SystemMonitor::gpuDescription() const {
    if (gpuAdapters_.empty()) {
        return "no display adapter";
    }
    std::string text;
    for (std::size_t index = 0; index < gpuAdapters_.size(); ++index) {
        if (index != 0) {
            text.append(", ");
        }
        text.append(gpuAdapters_[index].name);
    }
    return text;
}

void SystemMonitor::initGpuTelemetry() {
    discoverGpuAdapters();
    if (gpuTried_) {
        return;
    }
    gpuTried_ = true;
#if defined(_WIN32)
    if (gpuAdapters_.empty()) {
        // Already explained by discoverGpuAdapters; gpuAdapterSummary_ says why.
        return;
    }
    // "\GPU Engine(*)\Utilization Percentage" is published by Windows for every
    // vendor and is what Task Manager itself reads; it is derived from the same
    // vendor interfaces the vendor SDKs wrap.  That makes it the right source here,
    // where the vendor SDK for one particular vendor was never going to be.
    //
    // PdhAddEnglishCounterW, not PdhAddCounterW: the literal English path does not
    // exist on a localised Windows, and the disk counters further down this file
    // already make this point.
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || query == nullptr) {
        availabilityNote_.append(". GPU: the Windows GPU Engine counters are not present on this system");
        return;
    }
    gpuQuery_ = query;

    // One *wildcard* counter, added once.  It re-expands on every collect, so engine
    // instances that appear and disappear as processes come and go are picked up
    // without any add/remove churn - which also means the handle set cannot drift out
    // of step with reality, the way a hand-maintained list would.
    PDH_HCOUNTER counter = nullptr;
    if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                              &counter) != ERROR_SUCCESS ||
        counter == nullptr) {
        PdhCloseQuery(query);
        gpuQuery_ = nullptr;
        availabilityNote_.append(". GPU: the Windows GPU Engine counter could not be opened");
        return;
    }
    gpuCounter_ = counter;
    availabilityNote_.append(". GPU: name and utilization from the Windows GPU Engine counters");
#else
    availabilityNote_.append(". GPU telemetry is only implemented on Windows");
#endif
}

void SystemMonitor::shutdownGpuTelemetry() {
#if defined(_WIN32)
    if (gpuQuery_ != nullptr) {
        // Closing the query closes its counters; gpuCounter_ must not be removed
        // separately or it would be released twice.
        PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(gpuQuery_));
    }
#endif
    gpuCounter_ = nullptr;
    gpuQuery_ = nullptr;
    gpuTried_ = true;
    // The adapter list is deliberately kept: the adapters in a machine are a
    // permanent fact, and a shutdown must not make the next sample re-enumerate them.
}

MeasuredValue SystemMonitor::gpuPercent() {
    initGpuTelemetry();
    if (gpuAdapters_.empty()) {
        return MeasuredValue::unknown(gpuAdapterSummary_.empty()
                                          ? std::string("no display adapter was found on this machine")
                                          : gpuAdapterSummary_);
    }
#if defined(_WIN32)
    if (gpuQuery_ == nullptr || gpuCounter_ == nullptr) {
        return MeasuredValue::unknown("Windows reports " + gpuDescription() +
                                      ", but exposes no GPU Engine counter for it");
    }
    auto* query = reinterpret_cast<PDH_HQUERY>(gpuQuery_);
    auto* counter = reinterpret_cast<PDH_HCOUNTER>(gpuCounter_);
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return MeasuredValue::unknown("the GPU Engine counters could not be sampled");
    }

    // The array comes back with an instance name per item, which is what makes the
    // LUID filter below possible: the counter set is machine-wide, and only the
    // instance name says which adapter an engine belongs to.
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> items(64);
    DWORD bufferBytes = static_cast<DWORD>(items.size() * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
    DWORD itemCount = 0;
    PDH_STATUS read = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferBytes, &itemCount,
                                                    items.data());
    if (read == static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
        // The instance set grew between the collect and the read.  Sizing from the
        // byte count PDH just reported and trying once more is enough; a machine
        // growing the set again inside one sample is not a case worth looping on.
        const std::size_t needed = bufferBytes / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) + 1;
        items.resize(needed);
        bufferBytes = static_cast<DWORD>(items.size() * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        itemCount = 0;
        read = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferBytes, &itemCount,
                                             items.data());
    }
    if (read != static_cast<PDH_STATUS>(ERROR_SUCCESS)) {
        return MeasuredValue::unknown("the GPU Engine counters have no value yet");
    }

    // One adapter reports a separate percentage per engine (3D, video decode, video
    // processing, copy, ...).  Windows' own headline figure is the busiest engine,
    // and that is what is reported here: summing them can exceed 100 % and would be
    // read as a measurement when it is really an overlap of several.
    double busiest = 0.0;
    bool sawOurs = false;
    const std::size_t reported = itemCount < items.size() ? itemCount : items.size();
    for (std::size_t index = 0; index < reported; ++index) {
        const PDH_FMT_COUNTERVALUE_ITEM_W& item = items[index];
        bool ours = false;
        for (const GpuAdapter& adapter : gpuAdapters_) {
            if (std::wstring_view(item.szName).find(adapter.luidToken) != std::wstring_view::npos) {
                ours = true;
                break;
            }
        }
        if (!ours) {
            continue;
        }
        sawOurs = true;
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || item.FmtValue.doubleValue < 0.0) {
            continue;
        }
        if (item.FmtValue.doubleValue > busiest) {
            busiest = item.FmtValue.doubleValue;
        }
    }
    if (!sawOurs) {
        return MeasuredValue::unknown("no process is currently using " + gpuDescription());
    }
    if (busiest > 100.0) {
        busiest = 100.0;
    }
    return MeasuredValue::of(busiest);
#else
    return MeasuredValue::unknown("GPU utilization is only implemented on Windows");
#endif
}

void SystemMonitor::initDiskCounters() {
#if defined(_WIN32)
    if (diskQuery_ != nullptr || diskTried_) {
        return;
    }
    diskTried_ = true;
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        return;
    }
    if (PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &diskReadCounter_) !=
            ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &diskWriteCounter_) !=
            ERROR_SUCCESS) {
        PdhCloseQuery(query);
        return;
    }
    diskQuery_ = query;
#endif
}

MeasuredValue SystemMonitor::diskBytesPerSecond(bool read) {
#if defined(_WIN32)
    initDiskCounters();
    if (diskQuery_ == nullptr) {
        return MeasuredValue::unknown("the disk throughput counters are not present on this system");
    }
    // PdhGetFormattedCounterValue can return ERROR_SUCCESS while the value it filled in
    // carries CStatus PDH_CSTATUS_INVALID_DATA, which is exactly what the first sample
    // after a query is opened produces.  doubleValue is undefined then, so it is not
    // read - and a zero is never shown in its place, which would read as "idle disk".
    PDH_FMT_COUNTERVALUE value{};
    if (PdhGetFormattedCounterValue(read ? diskReadCounter_ : diskWriteCounter_, PDH_FMT_DOUBLE, nullptr,
                                    &value) != ERROR_SUCCESS) {
        return MeasuredValue::unknown("the disk counter value is not available yet");
    }
    if (value.CStatus != PDH_CSTATUS_VALID_DATA) {
        return MeasuredValue::unknown("the first disk sample establishes the baseline");
    }
    if (value.doubleValue < 0.0) {
        return MeasuredValue::unknown("the disk counter reported a negative rate");
    }
    return MeasuredValue::of(value.doubleValue);
#else
    (void)read;
    return MeasuredValue::unknown("system-wide disk counters are not implemented on this platform");
#endif
}
ResourceSample SystemMonitor::sample(std::int64_t mediaBytesWritten, double elapsedSeconds,
                                    double cpuSecondsUsedByJob) {
    (void)cpuSecondsUsedByJob;
    ResourceSample sample;
    sample.cpuPercent = cpuPercent();
    sample.memoryPercent = memoryPercent();
    sample.gpuPercent = gpuPercent();
#if defined(_WIN32)
    // One collection for both disk figures.  This used to call diskBytesPerSecond
    // twice, and each call collected the query itself, so the read and write numbers
    // were measured over two different windows a microsecond apart - they could not
    // agree with each other or with a graph drawn from either.  Collecting once makes
    // both describe the same interval and halves the PDH work per sample.
    initDiskCounters();
    if (diskQuery_ != nullptr) {
        PdhCollectQueryData(diskQuery_);
    }
#endif
    sample.diskReadBytesPerSecond = diskBytesPerSecond(true);
    sample.diskWriteBytesPerSecond = diskBytesPerSecond(false);
    if (elapsedSeconds > 0.5 && mediaBytesWritten >= 0) {
        // MonoSect's own number: bytes of published media per second.  It counts
        // only verified output, which is what makes it useful next to the disk
        // counters (the rest of the disk traffic is the source being read).
        sample.mediaThroughputBytesPerSecond =
            MeasuredValue::of(static_cast<double>(mediaBytesWritten) / elapsedSeconds);
    } else {
        sample.mediaThroughputBytesPerSecond =
            MeasuredValue::unknown("no media has been published yet in this sample window");
    }
    sample.sourceDescription = availabilityNote_;
    return sample;
}

} // namespace monosect::core
