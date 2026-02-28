#pragma once

#include "ui/Branding.hpp"
#include "engine/FFmpegProcess.hpp"
#include "engine/MediaProbe.hpp"
#include "engine/SplitEngine.hpp"
#include "engine/ThreadPool.hpp"
#include "engine/Types.hpp"
#include "engine/WatchFolder.hpp"
#include "monitor/HardwareDetector.hpp"
#include "monitor/SystemMonitor.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow;

namespace vsm {

class App final {
public:
    App();
    ~App();

    bool initialize();
    void run();

private:
    GLFWwindow* window_ = nullptr;

    SplitEngine splitEngine_;
    MediaProbe mediaProbe_;
    HardwareDetector hardwareDetector_;
    SystemMonitor systemMonitor_;
    ThreadPool threadPool_;
    WatchFolder watchFolder_;

    std::vector<std::filesystem::path> queue_;
    mutable std::mutex queueMutex_;
    int selectedQueueIndex_ = -1;

    std::vector<std::string> logs_;
    std::mutex logsMutex_;

    int splitModeIndex_ = 0;
    int precisionIndex_ = 0;
    int encoderIndex_ = 0;
    bool unrestrictedMode_ = false;
    bool watchFolderEnabled_ = false;
    bool showAbout_ = false;
    char splitInput_[64] = "60";
    char watchFolderInput_[512] = "";

    std::vector<std::string> encoderOptions_;
    TextureHandle fullLogoTexture_;

    std::array<float, 300> throughputPlot_{};
    std::array<float, 300> cpuPlot_{};
    std::array<float, 300> gpuPlot_{};
    int plotCursor_ = 0;

    std::atomic<double> throughputMBps_{ 0.0 };
    std::atomic<double> etaSeconds_{ 0.0 };
    SystemMetrics latestMetrics_{};
    std::chrono::steady_clock::time_point lastMetricsTick_{};
    bool dockLayoutInitialized_ = false;

    // Async FFmpeg bootstrap
    std::atomic<bool> ffmpegReady_{ false };
    std::atomic<bool> ffmpegBootstrapRunning_{ false };
    std::jthread ffmpegBootstrapThread_;
    std::filesystem::path exeDir_;

    void renderFrame();
    void renderDockspace();
    void setupDefaultDockLayout();
    void renderLeftPanel();
    void renderRightPanel();
    void renderBottomPanel();
    void renderMainMenu();

    void pushLog(const std::string& line);
    void addPath(const std::filesystem::path& path);
    void startSplitJobs();
    void removeSelectedFromQueue();
    [[nodiscard]] std::optional<std::filesystem::path> selectedQueuePath() const;
    [[nodiscard]] std::vector<std::filesystem::path> snapshotQueue() const;
    [[nodiscard]] std::vector<std::filesystem::path> openFilesDialog() const;
    SplitRequest makeRequest(const std::filesystem::path& path) const;
    void refreshMetrics();

    static void dropCallback(GLFWwindow* window, int count, const char** paths);
};

}