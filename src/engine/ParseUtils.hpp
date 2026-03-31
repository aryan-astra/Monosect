#pragma once

#include <cstdint>
#include <charconv>
#include <sstream>
#include <string>
#include <vector>

namespace vsm {

inline std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> parts;
    std::string item;
    std::istringstream ss(line);
    while (std::getline(ss, item, ',')) {
        parts.push_back(item);
    }
    return parts;
}

inline double parseDoubleOr(const std::string& value, double fallback) noexcept {
    if (value.empty()) {
        return fallback;
    }
    double parsed = 0.0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end) {
        return fallback;
    }
    return parsed;
}

inline int64_t parseInt64Or(const std::string& value, int64_t fallback) noexcept {
    if (value.empty()) {
        return fallback;
    }
    int64_t parsed = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end) {
        return fallback;
    }
    return parsed;
}

}
