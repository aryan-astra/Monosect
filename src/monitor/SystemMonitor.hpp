#pragma once

#include "engine/Types.hpp"

#include <cstdint>

namespace vsm {

class SystemMonitor final {
public:
    SystemMonitor();
    ~SystemMonitor();

    [[nodiscard]] SystemMetrics sample(double throughputMBps, double etaSeconds);

private:
    struct NvmlUtilization {
        unsigned int gpu;
        unsigned int memory;
    };
    using NvmlInitFn = int (*)();
    using NvmlShutdownFn = int (*)();
    using NvmlDeviceGetHandleByIndexFn = int (*)(unsigned int, void**);
    using NvmlDeviceGetUtilizationRatesFn = int (*)(void*, NvmlUtilization*);

    [[nodiscard]] double cpuUsagePercent();
    void initNvml();
    void shutdownNvml();
    [[nodiscard]] double queryNvmlGpuPercent() const;

    uint64_t previousIdle_ = 0;
    uint64_t previousKernel_ = 0;
    uint64_t previousUser_ = 0;
    bool hasBaseline_ = false;
    bool nvmlReady_ = false;
    void* nvmlLib_ = nullptr;
    void* nvmlDevice_ = nullptr;
    NvmlInitFn nvmlInitFn_ = nullptr;
    NvmlShutdownFn nvmlShutdownFn_ = nullptr;
    NvmlDeviceGetHandleByIndexFn nvmlDeviceGetHandleByIndexFn_ = nullptr;
    NvmlDeviceGetUtilizationRatesFn nvmlDeviceGetUtilizationRatesFn_ = nullptr;
};

}
