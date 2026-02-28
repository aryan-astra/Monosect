#include "monitor/SystemMonitor.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace vsm {

SystemMetrics SystemMonitor::sample(double throughputMBps, double etaSeconds) {
    SystemMetrics metrics;
    metrics.cpuUsagePercent = cpuUsagePercent();
    metrics.gpuUsagePercent = 0.0;
    metrics.diskReadMBps = 0.0;
    metrics.diskWriteMBps = 0.0;
    metrics.throughputMBps = throughputMBps;
    metrics.etaSeconds = etaSeconds;
    return metrics;
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