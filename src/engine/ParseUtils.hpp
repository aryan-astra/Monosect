#pragma once

#include <cstdint>
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
    try {
        return std::stod(value);
    } catch (...) {
        return fallback;
    }
}

inline int64_t parseInt64Or(const std::string& value, int64_t fallback) noexcept {
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

}
