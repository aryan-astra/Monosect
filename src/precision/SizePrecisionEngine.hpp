#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

namespace vsm {

class SizePrecisionEngine final {
public:
    struct SearchConfig {
        int64_t targetBytes = 0;
        int64_t toleranceBytes = 1024 * 1024;
        double minTimeSeconds = 0.0;
        double maxTimeSeconds = 0.0;
        std::size_t maxIterations = 24;
    };

    struct SearchResult {
        bool converged = false;
        double splitTimeSeconds = 0.0;
        int64_t resultingSizeBytes = 0;
        std::size_t iterations = 0;
    };

    using Evaluator = std::function<std::optional<int64_t>(double splitTimeSeconds)>;

    [[nodiscard]] SearchResult findSplitTime(const SearchConfig& config, const Evaluator& evaluator);

private:
    std::unordered_map<int64_t, int64_t> cache_;
};

}