#include "engine/FFmpegBootstrap.hpp"
#include "engine/ParseUtils.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        return false;
    }
    std::cout << "[OK] " << message << '\n';
    return true;
}

bool runParseUtilsChecks() {
    using vsm::parseDoubleOr;
    using vsm::parseInt64Or;
    using vsm::splitCsv;

    bool ok = true;

    const std::vector<std::string> csv = splitCsv("a,b,c");
    ok &= expect(csv.size() == 3 && csv[0] == "a" && csv[1] == "b" && csv[2] == "c", "splitCsv parses simple CSV");

    ok &= expect(parseDoubleOr("12.5", -1.0) == 12.5, "parseDoubleOr parses valid decimal");
    ok &= expect(parseDoubleOr("12.5x", -1.0) == -1.0, "parseDoubleOr falls back on invalid value");
    ok &= expect(parseDoubleOr("", 7.25) == 7.25, "parseDoubleOr falls back on empty value");

    ok &= expect(parseInt64Or("1024", -1) == 1024, "parseInt64Or parses valid integer");
    ok &= expect(parseInt64Or("10.24", -1) == -1, "parseInt64Or falls back on invalid value");
    ok &= expect(parseInt64Or("", 64) == 64, "parseInt64Or falls back on empty value");

    return ok;
}

bool runFFmpegBootstrapSmokeCheck() {
    namespace fs = std::filesystem;

    const fs::path base = fs::current_path() / "quality-checks-scratch";
    const fs::path appDirectory = base / "ffmpeg-bootstrap";
    const fs::path ffmpegPath = appDirectory / "ffmpeg.exe";
    const fs::path ffprobePath = appDirectory / "ffprobe.exe";

    std::error_code ec;
    fs::remove_all(base, ec);
    ec.clear();
    fs::create_directories(appDirectory, ec);
    if (ec) {
        std::cerr << "[FAIL] unable to create bootstrap smoke directory: " << ec.message() << '\n';
        return false;
    }

    std::ofstream(ffmpegPath, std::ios::binary).put('\0');
    std::ofstream(ffprobePath, std::ios::binary).put('\0');

    bool statusCallbackTriggered = false;
    const bool ready = vsm::ensureFFmpeg(appDirectory, [&](const std::string&) {
        statusCallbackTriggered = true;
    });

    bool ok = true;
    ok &= expect(ready, "ensureFFmpeg succeeds when binaries already exist");
    ok &= expect(!statusCallbackTriggered, "ensureFFmpeg fast path avoids bootstrap callback");

    fs::remove_all(base, ec);
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= runParseUtilsChecks();
    ok &= runFFmpegBootstrapSmokeCheck();
    return ok ? 0 : 1;
}
