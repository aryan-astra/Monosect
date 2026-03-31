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
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
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
    struct QueueEntry {
        uint64_t id = 0;
        std::filesystem::path path;
        double progress01 = 0.0;
        std::string status = "Queued";
        int estimatedChunks = 0;
        bool hasEstimate = false;
    };
    struct ProbeCacheEntry {
        uintmax_t sizeBytes = 0;
        std::filesystem::file_time_type modifiedTime{};
        MediaInfo info;
    };

    GLFWwindow* window_ = nullptr;

    SplitEngine splitEngine_;
    MediaProbe mediaProbe_;
    HardwareDetector hardwareDetector_;
    SystemMonitor systemMonitor_;
    ThreadPool threadPool_;
    WatchFolder watchFolder_;

    std::vector<QueueEntry> queue_;
    mutable std::mutex queueMutex_;
    int selectedQueueIndex_ = -1;
    uint64_t nextQueueEntryId_ = 1;

    std::vector<std::string> logs_;
    std::mutex logsMutex_;

    int splitModeIndex_ = 0;
    int precisionIndex_ = 0;
    int encoderIndex_ = 0;
    bool unrestrictedMode_ = false;
    bool watchFolderEnabled_ = false;
    bool autoTunerEnabledUi_ = true;
    bool showAbout_ = false;
    char splitInput_[64] = "60";
    char minFileSizeInput_[64] = "0";
    char watchFolderInput_[512] = "";
    int maxWorkerLimit_ = std::max(1u, std::thread::hardware_concurrency());
    int hardwareWorkerCount_ = std::max(1u, std::thread::hardware_concurrency());

    std::vector<std::string> encoderOptions_;
    mutable std::mutex encoderMutex_;
    mutable std::mutex probeCacheMutex_;
    std::unordered_map<std::string, ProbeCacheEntry> probeCache_;
    TextureHandle fullLogoTexture_;

    std::array<float, 300> throughputPlot_{};
    std::array<float, 300> cpuPlot_{};
    std::array<float, 300> gpuPlot_{};
    int plotCursor_ = 0;

    std::atomic<double> throughputMBps_{ 0.0 };
    std::atomic<double> latestCpuUsage_{ 0.0 };
    std::atomic<double> etaSeconds_{ 0.0 };
    std::atomic<bool> autoTunerEnabled_{ true };
    SystemMetrics latestMetrics_{};
    std::chrono::steady_clock::time_point lastMetricsTick_{};
    bool dockLayoutInitialized_ = false;
    std::atomic<bool> splitInProgress_{ false };
    std::atomic<int> totalSplitJobs_{ 0 };
    std::atomic<int> completedSplitJobs_{ 0 };
    std::atomic<double> currentJobProgress01_{ 0.0 };
    std::string currentJobStatus_;
    std::filesystem::path currentJobPath_;
    mutable std::mutex splitStatusMutex_;
    std::atomic<bool> splitPauseRequested_{ false };
    std::atomic<bool> splitCancelRequested_{ false };
    std::atomic<int64_t> minFileSizeBytes_{ 0 };
    std::atomic<int64_t> currentTargetBytes_{ 0 };
    std::atomic<int> tunedWorkerLimit_{ 1 };
    std::atomic<int> tunedMaxWorkers_{ 1 };
    std::atomic<int> activeSplitJobs_{ 0 };
    std::atomic<int> splitSucceededJobs_{ 0 };
    std::atomic<int> splitFailedJobs_{ 0 };
    std::atomic<int> splitCanceledJobs_{ 0 };
    std::atomic<double> averageThroughputMBps_{ 0.0 };
    mutable std::mutex throughputStatsMutex_;
    double throughputSumMBps_ = 0.0;
    int throughputSamples_ = 0;
    mutable std::mutex splitControlMutex_;
    std::condition_variable splitControlCv_;
    mutable std::mutex splitConcurrencyMutex_;
    std::condition_variable splitConcurrencyCv_;
    std::jthread autoTuneThread_;

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
    void updateResponsiveScale();

    void pushLog(const std::string& line);
    [[nodiscard]] std::optional<MediaInfo> probeMediaCached(const std::filesystem::path& path);
    void resetThroughputStats();
    void recordJobThroughput(double throughputMBps);
    void addPath(const std::filesystem::path& path);
    void addFileToQueue(const std::filesystem::path& path);
    void startSplitJobs();
    void runDryRunEstimate();
    void removeSelectedFromQueue();
    [[nodiscard]] std::optional<std::filesystem::path> selectedQueuePath() const;
    [[nodiscard]] std::vector<QueueEntry> snapshotQueueEntries() const;
    [[nodiscard]] std::vector<std::filesystem::path> openFilesDialog() const;
    SplitRequest makeRequest(const std::filesystem::path& path) const;
    [[nodiscard]] std::string selectedEncoderValue() const;
    void refreshMetrics();
    void updateQueueEntry(uint64_t id, double progress01, const std::string& status, bool clearEstimate = false);
    void updateQueueEstimate(uint64_t id, int estimatedChunks, const std::string& status);
    [[nodiscard]] bool waitWhilePausedOrCanceled();
    [[nodiscard]] bool shouldPauseSplit() const;
    [[nodiscard]] bool shouldCancelSplit() const;
    [[nodiscard]] bool acquireConcurrencySlot();
    void releaseConcurrencySlot();
    void runAutoTuner(std::stop_token token);
    [[nodiscard]] int64_t parseMegabytesToBytes(const char* text) const;
    void setCurrentSplitStatus(const std::filesystem::path& path, double progress01, const std::string& status);
    [[nodiscard]] std::pair<std::filesystem::path, std::string> currentSplitStatusSnapshot() const;
    [[nodiscard]] double overallSplitProgress01() const;

    static void dropCallback(GLFWwindow* window, int count, const char** paths);
};

}
