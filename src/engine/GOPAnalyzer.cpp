#include "engine/GOPAnalyzer.hpp"
#include "engine/ParseUtils.hpp"

#include <cmath>
#include <limits>
#include <sstream>

namespace vsm {

GOPAnalyzer::GOPAnalyzer(FFmpegProcess process)
    : process_(std::move(process)) {}

GOPAnalysis GOPAnalyzer::analyze(const std::filesystem::path& path) const {
    GOPAnalysis analysis;

    const auto result = process_.runFFprobe({
        "-v", "error",
        "-select_streams", "v:0",
        "-show_frames",
        "-show_entries", "frame=key_frame,pkt_pts_time,pkt_dts_time,pkt_pos,best_effort_timestamp_time",
        "-of", "csv=p=0",
        path.string()
    });

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

        const bool keyframe = cols[0] == "1";
        if (!keyframe) {
            continue;
        }

        const double ptsTime = cols.size() > 1 ? parseDoubleOr(cols[1], 0.0) : 0.0;
        const double dtsTime = cols.size() > 2 ? parseDoubleOr(cols[2], ptsTime) : ptsTime;
        const int64_t pos = cols.size() > 3 ? parseInt64Or(cols[3], 0) : 0;

        GOPEntry entry;
        entry.pts = static_cast<int64_t>(ptsTime * 1000000.0);
        entry.dts = static_cast<int64_t>(dtsTime * 1000000.0);
        entry.timeSeconds = ptsTime;
        entry.byteOffset = pos;
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

std::optional<GOPEntry> GOPAnalyzer::nearestKeyframe(const GOPAnalysis& analysis, double timeSeconds) const {
    if (analysis.entries.empty()) {
        return std::nullopt;
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
