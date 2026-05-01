#include "monitor/SystemMonitor.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace vsm {

namespace {
constexpr int kNvmlSuccess = 0;
} // namespace

SystemMonitor::SystemMonitor() {
    initNvml();
}

SystemMonitor::~SystemMonitor() {
    shutdownNvml();
}

SystemMetrics SystemMonitor::sample(double throughputMBps, double etaSeconds) {
    SystemMetrics metrics;
    metrics.cpuUsagePercent = cpuUsagePercent();
    metrics.gpuUsagePercent = queryNvmlGpuPercent();
    metrics.diskReadMBps = 0.0;
    metrics.diskWriteMBps = 0.0;
    metrics.throughputMBps = throughputMBps;
    metrics.etaSeconds = etaSeconds;
    return metrics;
}

void SystemMonitor::initNvml() {
    if (nvmlReady_) {
        return;
    }

    HMODULE library = LoadLibraryA("nvml.dll");
    if (library == nullptr) {
        return;
    }

    auto initFn = reinterpret_cast<NvmlInitFn>(GetProcAddress(library, "nvmlInit_v2"));
    auto shutdownFn = reinterpret_cast<NvmlShutdownFn>(GetProcAddress(library, "nvmlShutdown"));
    auto getHandleFn = reinterpret_cast<NvmlDeviceGetHandleByIndexFn>(GetProcAddress(library, "nvmlDeviceGetHandleByIndex_v2"));
    auto getUtilFn = reinterpret_cast<NvmlDeviceGetUtilizationRatesFn>(GetProcAddress(library, "nvmlDeviceGetUtilizationRates"));
    if (initFn == nullptr || shutdownFn == nullptr || getHandleFn == nullptr || getUtilFn == nullptr) {
        FreeLibrary(library);
        return;
    }
    if (initFn() != kNvmlSuccess) {
        FreeLibrary(library);
        return;
    }

    void* device = nullptr;
    if (getHandleFn(0, &device) != kNvmlSuccess || device == nullptr) {
        shutdownFn();
        FreeLibrary(library);
        return;
    }

    nvmlLib_ = library;
    nvmlDevice_ = device;
    nvmlInitFn_ = initFn;
    nvmlShutdownFn_ = shutdownFn;
    nvmlDeviceGetHandleByIndexFn_ = getHandleFn;
    nvmlDeviceGetUtilizationRatesFn_ = getUtilFn;
    nvmlReady_ = true;
}

void SystemMonitor::shutdownNvml() {
    if (nvmlLib_ == nullptr) {
        return;
    }

    if (nvmlShutdownFn_ != nullptr) {
        nvmlShutdownFn_();
    }

    FreeLibrary(static_cast<HMODULE>(nvmlLib_));
    nvmlReady_ = false;
    nvmlLib_ = nullptr;
    nvmlDevice_ = nullptr;
    nvmlInitFn_ = nullptr;
    nvmlShutdownFn_ = nullptr;
    nvmlDeviceGetHandleByIndexFn_ = nullptr;
    nvmlDeviceGetUtilizationRatesFn_ = nullptr;
}

double SystemMonitor::queryNvmlGpuPercent() const {
    if (!nvmlReady_ || nvmlDevice_ == nullptr || nvmlDeviceGetUtilizationRatesFn_ == nullptr) {
        return 0.0;
    }

    NvmlUtilization utilization{};
    if (nvmlDeviceGetUtilizationRatesFn_(nvmlDevice_, &utilization) != kNvmlSuccess) {
        return 0.0;
    }

    return std::clamp(static_cast<double>(utilization.gpu), 0.0, 100.0);
}

double SystemMonitor::cpuUsagePercent() {
    FILETIME idleTime;
    FILETIME kernelTime;
    FILETIME userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime) == 0) {
        return 0.0;
    }

    auto toUInt64 = [](const FILETIME& ft) -> uint64_t {
        ULARGE_INTEGER value;
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return value.QuadPart;
    };

    const uint64_t idle = toUInt64(idleTime);
    const uint64_t kernel = toUInt64(kernelTime);
    const uint64_t user = toUInt64(userTime);

    if (!hasBaseline_) {
        previousIdle_ = idle;
        previousKernel_ = kernel;
        previousUser_ = user;
        hasBaseline_ = true;
        return 0.0;
    }

    const uint64_t idleDiff = idle - previousIdle_;
    const uint64_t kernelDiff = kernel - previousKernel_;
    const uint64_t userDiff = user - previousUser_;

    previousIdle_ = idle;
    previousKernel_ = kernel;
    previousUser_ = user;

    const uint64_t total = kernelDiff + userDiff;
    if (total == 0) {
        return 0.0;
    }
    const double usage = (1.0 - static_cast<double>(idleDiff) / static_cast<double>(total)) * 100.0;
    return std::clamp(usage, 0.0, 100.0);
}

}
