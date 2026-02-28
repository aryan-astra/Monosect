#include "precision/SizePrecisionEngine.hpp"

#include <cstdlib>
#include <cmath>
#include <limits>

namespace vsm {

SizePrecisionEngine::SearchResult SizePrecisionEngine::findSplitTime(const SearchConfig& config, const Evaluator& evaluator) {
    SearchResult result;

    double low = config.minTimeSeconds;
    double high = config.maxTimeSeconds;

    auto getSize = [&](double splitTime) -> std::optional<int64_t> {
        const int64_t key = static_cast<int64_t>(std::llround(splitTime * 1000.0));
        if (const auto it = cache_.find(key); it != cache_.end()) {
            return it->second;
        }
        const auto measured = evaluator(splitTime);
        if (measured.has_value()) {
            cache_[key] = *measured;
        }
        return measured;
    };

    int64_t bestDistance = std::numeric_limits<int64_t>::max();

    for (std::size_t iteration = 0; iteration < config.maxIterations; ++iteration) {
        const double mid = low + ((high - low) / 2.0);
        const auto measured = getSize(mid);
        if (!measured.has_value()) {
            break;
        }

        const int64_t distance = std::llabs(*measured - config.targetBytes);
        if (distance < bestDistance) {
            bestDistance = distance;
            result.splitTimeSeconds = mid;
            result.resultingSizeBytes = *measured;
        }

        result.iterations = iteration + 1;
        if (distance <= config.toleranceBytes) {
            result.converged = true;
            return result;
        }

        if (*measured > config.targetBytes) {
            high = mid;
        } else {
            low = mid;
        }
    }

    return result;
}

}