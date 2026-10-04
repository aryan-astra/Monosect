#include "monitor/SystemMonitor.hpp"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
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
        case MetricSource::Nvml: return "NVIDIA NVML";
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
}

SystemMonitor::~SystemMonitor() {
    shutdownGpuTelemetry();
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

void SystemMonitor::initGpuTelemetry() {
    if (gpuReady_ || gpuLibrary_ != nullptr) {
        return;
    }
#if defined(_WIN32)
    // NVML ships with the driver; it is loaded dynamically so MonoSect runs
    // without an NVIDIA card, and its absence is not an error.
    HMODULE library = LoadLibraryW(L"nvml.dll");
    if (library == nullptr) {
        availabilityNote_.append(". GPU telemetry: NVML not present (no NVIDIA driver found)");
        return;
    }
    gpuLibrary_ = reinterpret_cast<void*>(library);
    nvmlInit_ = reinterpret_cast<NvmlInitFn>(
        reinterpret_cast<void*>(GetProcAddress(library, "nvmlInit_v2")));
    nvmlShutdown_ = reinterpret_cast<NvmlShutdownFn>(
        reinterpret_cast<void*>(GetProcAddress(library, "nvmlShutdown")));
    nvmlHandle_ = reinterpret_cast<NvmlHandleFn>(
        reinterpret_cast<void*>(GetProcAddress(library, "nvmlDeviceGetHandleByIndex_v2")));
    nvmlUtil_ = reinterpret_cast<NvmlUtilFn>(
        reinterpret_cast<void*>(GetProcAddress(library, "nvmlDeviceGetUtilizationRates")));
    if (nvmlInit_ == nullptr || nvmlHandle_ == nullptr || nvmlUtil_ == nullptr) {
        availabilityNote_.append(". GPU telemetry: the NVIDIA driver does not expose utilization");
        FreeLibrary(library);
        gpuLibrary_ = nullptr;
        return;
    }
    if (nvmlInit_() != 0) {
        availabilityNote_.append(". GPU telemetry: NVML could not be initialised");
        FreeLibrary(library);
        gpuLibrary_ = nullptr;
        return;
    }
    if (nvmlHandle_(0, &gpuDevice_) != 0) {
        availabilityNote_.append(". GPU telemetry: no NVIDIA GPU found at index 0");
        nvmlShutdown_();
        FreeLibrary(library);
        gpuLibrary_ = nullptr;
        return;
    }
    gpuReady_ = true;
    availabilityNote_.append(". GPU from NVIDIA NVML");
#else
    availabilityNote_.append(". GPU telemetry is only implemented for NVIDIA GPUs on Windows");
#endif
}

void SystemMonitor::shutdownGpuTelemetry() {
#if defined(_WIN32)
    if (gpuReady_ && nvmlShutdown_ != nullptr) {
        nvmlShutdown_();
    }
    if (gpuLibrary_ != nullptr) {
        FreeLibrary(reinterpret_cast<HMODULE>(gpuLibrary_));
    }
#endif
    gpuReady_ = false;
    gpuLibrary_ = nullptr;
    gpuDevice_ = nullptr;
}

MeasuredValue SystemMonitor::gpuPercent() {
    initGpuTelemetry();
    if (!gpuReady_) {
        return MeasuredValue::unknown("no GPU utilization interface available"
                                      " (NVIDIA NVML is the only one implemented; AMD and Intel report n/a rather "
                                      "than a guessed number)");
    }
#if defined(_WIN32)
    NvmlUtilization utilization{};
    if (nvmlUtil_(gpuDevice_, &utilization) != 0) {
        return MeasuredValue::unknown("NVML refused the utilization query");
    }
    return MeasuredValue::of(static_cast<double>(utilization.gpu));
#else
    return MeasuredValue::unknown("GPU utilization is only implemented on Windows");
#endif
}

MeasuredValue SystemMonitor::diskBytesPerSecond(bool read) {
#if defined(_WIN32)
    // PDH is queried through a single counter pair, sampled at least 1 s apart by
    // the caller.  Querying PDH on every frame would be far more expensive than
    // the graph is worth, so the sample rate is the UI's business and is 1 Hz.
    static PDH_HQUERY query = nullptr;
    static PDH_HCOUNTER readCounter = nullptr;
    static PDH_HCOUNTER writeCounter = nullptr;
    static bool initialized = false;
    static PDH_FMT_COUNTERVALUE readValue{};
    static PDH_FMT_COUNTERVALUE writeValue{};
    if (!initialized) {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            return MeasuredValue::unknown("the performance counters could not be opened (PdhOpenQuery)");
        }
        if (PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &readCounter) !=
                ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &writeCounter) !=
                ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return MeasuredValue::unknown("the disk throughput counters are not present on this system");
        }
        PdhCollectQueryData(query);
        initialized = true;
        return MeasuredValue::unknown("the first sample establishes the baseline");
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return MeasuredValue::unknown("the disk counters could not be sampled");
    }
    if (PdhGetFormattedCounterValue(read ? readCounter : writeCounter, PDH_FMT_DOUBLE, nullptr,
                                    read ? &readValue : &writeValue) != ERROR_SUCCESS) {
        return MeasuredValue::unknown("the disk counter value is not available yet");
    }
    const double value = read ? readValue.doubleValue : writeValue.doubleValue;
    if (value < 0.0) {
        return MeasuredValue::unknown("the disk counter reported a negative rate");
    }
    return MeasuredValue::of(value);
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
