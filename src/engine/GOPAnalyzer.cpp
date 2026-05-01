#include "engine/GOPAnalyzer.hpp"
#include "engine/ParseUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace vsm {

GOPAnalyzer::GOPAnalyzer(FFmpegProcess process)
    : process_(std::move(process)) {}

GOPAnalysis GOPAnalyzer::analyze(const std::filesystem::path& path, double nearSeconds, double windowSeconds) const {
    GOPAnalysis analysis;

    std::vector<std::string> args = {
        "-v", "error",
        "-select_streams", "v:0",
    };

    if (nearSeconds >= 0.0 && std::isfinite(nearSeconds) && windowSeconds > 0.0 && std::isfinite(windowSeconds)) {
        const double intervalStart = std::max(0.0, nearSeconds - windowSeconds);
        const double intervalEnd = std::max(intervalStart, nearSeconds + windowSeconds);
        const std::string readInterval = "%" + std::to_string(intervalStart) + "%" + std::to_string(intervalEnd);
        args.push_back("-read_intervals");
        args.push_back(readInterval);
    }

    args.push_back("-show_entries");
    args.push_back("packet=pts_time,flags");
    args.push_back("-of");
    args.push_back("csv=p=0");
    args.push_back(path.string());

    const auto result = process_.runFFprobe(args);

    if (!result.ok) {
        return analysis;
    }

    std::istringstream lines(result.stdOut);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        const auto cols = splitCsv(line);
        if (cols.empty()) {
            continue;
        }

        if (cols.size() < 2) {
            continue;
        }

        std::string ptsToken;
        std::string flagsToken;
        if (cols.size() >= 3 && cols[0] == "packet") {
            ptsToken = cols[1];
            flagsToken = cols[2];
        } else {
            ptsToken = cols[0];
            flagsToken = cols[1];
        }

        if (flagsToken.find('K') == std::string::npos) {
            continue;
        }

        const double ptsTime = parseDoubleOr(ptsToken, -1.0);
        if (ptsTime < 0.0) {
            continue;
        }

        GOPEntry entry;
        entry.pts = static_cast<int64_t>(ptsTime * 1000000.0);
        entry.dts = entry.pts;
        entry.timeSeconds = ptsTime;
        analysis.entries.push_back(entry);
    }

    if (analysis.entries.size() >= 2) {
        double totalInterval = 0.0;
        for (std::size_t i = 1; i < analysis.entries.size(); ++i) {
            totalInterval += (analysis.entries[i].timeSeconds - analysis.entries[i - 1].timeSeconds);
        }
        analysis.averageIntervalSeconds = totalInterval / static_cast<double>(analysis.entries.size() - 1);
    }

    if (!analysis.entries.empty()) {
        const double durationSeconds = analysis.entries.back().timeSeconds;
        if (durationSeconds > 0.0) {
            analysis.densityPerMinute = static_cast<double>(analysis.entries.size()) / (durationSeconds / 60.0);
        }
    }

    analysis.preciseLosslessPossible = !analysis.entries.empty();
    return analysis;
}

std::optional<GOPEntry> GOPAnalyzer::nearestKeyframe(const GOPAnalysis& analysis,
                                                     double timeSeconds,
                                                     KeyframeSearchDir dir) const {
    if (analysis.entries.empty()) {
        return std::nullopt;
    }

    if (dir == KeyframeSearchDir::AtOrBefore) {
        for (int i = static_cast<int>(analysis.entries.size()) - 1; i >= 0; --i) {
            if (analysis.entries[static_cast<std::size_t>(i)].timeSeconds <= timeSeconds) {
                return analysis.entries[static_cast<std::size_t>(i)];
            }
        }
        return analysis.entries.front();
    }

    if (dir == KeyframeSearchDir::AtOrAfter) {
        for (const auto& entry : analysis.entries) {
            if (entry.timeSeconds >= timeSeconds) {
                return entry;
            }
        }
        return analysis.entries.back();
    }

    std::size_t bestIndex = 0;
    double bestDelta = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < analysis.entries.size(); ++i) {
        const double delta = std::abs(analysis.entries[i].timeSeconds - timeSeconds);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = i;
        }
    }
    return analysis.entries[bestIndex];
}

}
