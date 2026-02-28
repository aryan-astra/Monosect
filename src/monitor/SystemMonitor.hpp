#pragma once

#include "engine/Types.hpp"

#include <cstdint>

namespace vsm {

class SystemMonitor final {
public:
    [[nodiscard]] SystemMetrics sample(double throughputMBps, double etaSeconds);

private:
    [[nodiscard]] double cpuUsagePercent();

    uint64_t previousIdle_ = 0;
    uint64_t previousKernel_ = 0;
    uint64_t previousUser_ = 0;
    bool hasBaseline_ = false;
};

}