#include "core/ResourcePolicy.hpp"

#include <algorithm>
#include <sstream>
#include <thread>

namespace monosect::core {

namespace {

// Windows reserves some headroom; keeping one logical core free for the UI and
// the storage stack avoids the classic "UI freezes while encoding" complaint.
constexpr int kUiHeadroomCores = 1;

} // namespace

const char* resourceModeName(ResourceMode mode) noexcept {
    switch (mode) {
        case ResourceMode::Conservative: return "Conservative";
        case ResourceMode::Balanced: return "Balanced";
        case ResourceMode::Maximum: return "Maximum";
        case ResourceMode::Custom: return "Custom";
    }
    return "Balanced";
}

std::string resourceModeDescription(ResourceMode mode) {
    switch (mode) {
        case ResourceMode::Conservative:
            return "Keeps about half of the physical cores free for other work and runs FFmpeg at normal "
                   "priority. Slowest, quietest, safest on a laptop on battery.";
        case ResourceMode::Balanced:
            return "Uses the physical cores for encoding and leaves the rest for the rest of Windows. This is "
                   "the default because it finishes at essentially the same speed as Maximum on most machines.";
        case ResourceMode::Maximum:
            return "Uses every logical core and raises the FFmpeg priority. Useful for long jobs on a machine you "
                   "are not using. It does not guarantee a faster job: stream copying is limited by disk "
                   "throughput, not by the CPU.";
        case ResourceMode::Custom:
            return "Explicit thread counts. MonoSect still enforces one active job at a time.";
    }
    return {};
}

CpuTopology CpuTopology::detect() {
    CpuTopology topology;
    const unsigned int logical = std::thread::hardware_concurrency();
    topology.logicalCores = logical > 0 ? static_cast<int>(logical) : 1;
    // A portable fallback: assume SMT is present (2 logical cores per physical
    // core) unless the caller on Windows supplies the real topology. The Windows
    // implementation replaces this with
    // GetLogicalProcessorInformationEx(RelationProcessorCore).
    topology.physicalCores = std::max(1, topology.logicalCores / 2);
    return topology;
}

std::string CpuTopology::describe() const {
    std::ostringstream out;
    out << physicalCores << " physical / " << logicalCores << " logical cores";
    if (hybrid) {
        out << " (" << performanceCores << " performance, " << efficiencyCores << " efficiency)";
    }
    return out.str();
}

ResourcePolicy deriveResourcePolicy(const ResourcePolicyInput& input) {
    ResourcePolicy policy;
    policy.mode = input.mode;

    const int logical = std::max(1, input.topology.logicalCores);
    const int physical = std::max(1, input.topology.physicalCores);
    std::ostringstream rationale;

    switch (input.mode) {
        case ResourceMode::Conservative:
            policy.encoderThreads = std::max(1, physical / 2);
            policy.filterThreads = policy.encoderThreads;
            policy.priorityDelta = -1;
            policy.useHardwareDecoding = input.userEnabledHardwareDecoding && input.gpu.encoderPresent;
            policy.useGpuEncoder = false;
            rationale << "Conservative: " << policy.encoderThreads
                      << " encoder threads (half of the physical cores), normal priority.";
            break;

        case ResourceMode::Balanced:
            policy.encoderThreads = std::max(1, physical);
            policy.filterThreads = std::max(1, physical);
            policy.priorityDelta = -1;
            policy.useHardwareDecoding = input.userEnabledHardwareDecoding;
            policy.useGpuEncoder = input.userEnabledGpu && input.gpu.encoderUsable && input.jobWillEncode;
            if (policy.useGpuEncoder) {
                policy.gpuEncoder = input.gpu.usableEncoder;
            }
            rationale << "Balanced: " << policy.encoderThreads
                      << " encoder threads (one per physical core), FFmpeg below normal priority.";
            break;

        case ResourceMode::Maximum:
            policy.encoderThreads = logical;
            policy.filterThreads = logical;
            policy.priorityDelta = 1;
            policy.useHardwareDecoding = input.userEnabledHardwareDecoding;
            policy.useGpuEncoder = input.userEnabledGpu && input.gpu.encoderUsable && input.jobWillEncode;
            if (policy.useGpuEncoder) {
                policy.gpuEncoder = input.gpu.usableEncoder;
            }
            rationale << "Maximum: " << policy.encoderThreads
                      << " encoder threads, above normal priority.";
            break;

        case ResourceMode::Custom:
            policy.encoderThreads = std::max(1, input.topology.logicalCores);
            policy.filterThreads = policy.encoderThreads;
            policy.priorityDelta = 0;
            rationale << "Custom: values supplied by the user.";
            break;
    }

    if (input.jobWillEncode && input.userEnabledGpu && input.gpu.encoderPresent && !input.gpu.encoderUsable) {
        rationale << " The GPU encoder is present but was refused by the capability probe ("
                  << (input.gpu.probeDetail.empty() ? "unknown reason" : input.gpu.probeDetail)
                  << "), so the software encoder is used. A listed encoder is not a usable encoder.";
    }
    if (input.jobWillEncode && input.userEnabledGpu && input.gpu.encoderUsable && !policy.useGpuEncoder &&
        input.mode == ResourceMode::Conservative) {
        rationale << " The GPU encoder is available but not used in Conservative mode.";
    }
    if (!input.jobWillEncode) {
        rationale << " This plan only copies packets, so thread settings do not affect the result.";
    }
    if (input.destinationThroughputBytesPerSecond > 0) {
        rationale << " Destination throughput measured at "
                  << (input.destinationThroughputBytesPerSecond / (1024 * 1024)) << " MiB/s.";
    }

    policy.concurrentSegments = 1;
    policy.rationale = rationale.str();
    return policy;
}

ConcurrencyDecision decideSegmentConcurrency(const ResourcePolicyInput& input,
                                             bool sourceAndDestinationOnSameVolume) {
    ConcurrencyDecision decision;
    decision.processes = 1;

    if (sourceAndDestinationOnSameVolume) {
        decision.reason =
            "The source and the destination are on the same volume. Writing several parts at once makes the "
            "drive seek between them and slows the whole job down, so MonoSect writes one part at a time.";
        return decision;
    }

    if (input.destinationThroughputBytesPerSecond > 0 &&
        input.destinationThroughputBytesPerSecond < 200LL * 1024 * 1024) {
        decision.reason =
            "The destination device measured below 200 MiB/s. A single FFmpeg process already saturates it, so "
            "extra processes would only add contention.";
        return decision;
    }

    decision.reason =
        "One part at a time. Stream copying is normally limited by the storage device, and one process per "
        "device keeps the throughput predictable and the parts verifiable in order.";
    return decision;
}

} // namespace monosect::core
