#include "ui/App.hpp"

#include "ui/Branding.hpp"
#include "engine/FFmpegBootstrap.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iterator>
#include <sstream>
#include <cwchar>

namespace vsm {

App::App()
    : threadPool_(std::max(1u, std::thread::hardware_concurrency())) {}

App::~App() {
    if (ffmpegBootstrapThread_.joinable()) {
        ffmpegBootstrapThread_.request_stop();
        ffmpegBootstrapThread_.join();
    }
    watchFolder_.stop();
    releaseTexture(fullLogoTexture_);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

bool App::initialize() {
    if (!glfwInit()) {
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window_ = glfwCreateWindow(1600, 960, "Monosect", nullptr, nullptr);
    if (window_ == nullptr) {
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    // ---- Custom Professional Dark Theme ----
    auto& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.PopupRounding     = 4.0f;
    style.ChildRounding     = 4.0f;
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(8.0f, 5.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 12.0f;
    style.WindowBorderSize  = 1.0f;

    auto* c = style.Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.09f, 0.09f, 0.09f, 0.96f);
    c[ImGuiCol_Border]                = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.07f, 0.07f, 0.07f, 1.0f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.07f, 0.07f, 0.07f, 1.0f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.07f, 0.07f, 0.07f, 1.0f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.07f, 0.07f, 0.07f, 0.6f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.38f, 0.38f, 0.38f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    c[ImGuiCol_Button]                = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.26f, 0.26f, 0.26f, 1.0f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.34f, 0.34f, 0.34f, 1.0f);
    c[ImGuiCol_Header]                = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    c[ImGuiCol_Separator]             = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.30f, 0.30f, 0.30f, 0.25f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.45f, 0.45f, 0.45f, 0.60f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.60f, 0.60f, 0.60f, 0.90f);
    c[ImGuiCol_Tab]                   = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.30f, 0.30f, 0.30f, 0.80f);
    c[ImGuiCol_DockingPreview]        = ImVec4(0.50f, 0.50f, 0.50f, 0.40f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.07f, 0.07f, 0.07f, 1.0f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.40f, 0.40f, 0.40f, 0.35f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);

    // DPI-aware font loading (falls back to built-in font if unavailable)
    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    if (xscale > 1.0f) {
        style.ScaleAllSizes(xscale);
    }
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", std::round(16.0f * xscale));

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    glfwSetWindowUserPointer(window_, this);
    glfwSetDropCallback(window_, &App::dropCallback);

    const auto exeDir = executableDirectory();
    const auto squareIconPath = exeDir / "Monosect-icon-square.png";
    const auto fullLogoPath = exeDir / "Monosect-full-logo.png";

    // Use the square-cropped icon for window titlebar + taskbar
    ImageRgba iconPixels;
    if (loadPngAsRgba(squareIconPath, iconPixels) && iconPixels.valid()) {
        // Provide multiple sizes for crisp display at all DPI levels
        std::vector<GLFWimage> glfwImages;
        std::vector<ImageRgba> scaledIcons;
        constexpr int iconSizes[] = {16, 32, 48, 64, 256};
        for (int sz : iconSizes) {
            ImageRgba scaled;
            scaled.width = sz;
            scaled.height = sz;
            scaled.pixels.resize(static_cast<size_t>(sz) * sz * 4);
            // Nearest-neighbour downsample (simple and fast)
            for (int y = 0; y < sz; ++y) {
                for (int x = 0; x < sz; ++x) {
                    const int srcX = x * iconPixels.width / sz;
                    const int srcY = y * iconPixels.height / sz;
                    const auto* sp = &iconPixels.pixels[(static_cast<size_t>(srcY) * iconPixels.width + srcX) * 4];
                    auto* dp = &scaled.pixels[(static_cast<size_t>(y) * sz + x) * 4];
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
                }
            }
            scaledIcons.push_back(std::move(scaled));
        }
        for (auto& ic : scaledIcons) {
            GLFWimage gi{};
            gi.width = ic.width;
            gi.height = ic.height;
            gi.pixels = ic.pixels.data();
            glfwImages.push_back(gi);
        }
        glfwSetWindowIcon(window_, static_cast<int>(glfwImages.size()), glfwImages.data());
    }

    // Only load full logo texture for the About dialog
    if (!loadPngAsTexture(fullLogoPath, fullLogoTexture_)) {
        pushLog("Branding warning: unable to load Monosect-full-logo.png");
    }

    // Start FFmpeg bootstrap asynchronously so the UI renders immediately
    exeDir_ = exeDir;
    encoderOptions_.push_back("copy"); // default until detection finishes
    ffmpegBootstrapRunning_.store(true);
    ffmpegBootstrapThread_ = std::jthread([this](std::stop_token) {
        if (!ensureFFmpeg(exeDir_, [this](const std::string& msg) { pushLog(msg); })) {
            pushLog("Warning: FFmpeg not available. Split/merge operations will fail.");
            pushLog("Please place ffmpeg.exe and ffprobe.exe next to Monosect.exe");
            ffmpegBootstrapRunning_.store(false);
            return;
        }

        // Configure engines to use local FFmpeg binaries
        const auto ffmpegBin  = (exeDir_ / "ffmpeg.exe").string();
        const auto ffprobeBin = (exeDir_ / "ffprobe.exe").string();
        FFmpegProcess localProcess(ffmpegBin, ffprobeBin);
        splitEngine_    = SplitEngine(localProcess);
        mediaProbe_     = MediaProbe(localProcess);
        hardwareDetector_ = HardwareDetector(localProcess);

        const auto enc = hardwareDetector_.detect();
        {
            std::scoped_lock lock(logsMutex_);
            encoderOptions_.clear();
            if (enc.h264Nvenc) encoderOptions_.push_back("h264_nvenc");
            if (enc.hevcNvenc) encoderOptions_.push_back("hevc_nvenc");
            if (enc.h264Qsv) encoderOptions_.push_back("h264_qsv");
            if (enc.h264Amf) encoderOptions_.push_back("h264_amf");
            encoderOptions_.push_back("copy");
        }

        ffmpegReady_.store(true);
        ffmpegBootstrapRunning_.store(false);
        pushLog("FFmpeg ready.");
    });

    lastMetricsTick_ = std::chrono::steady_clock::now();
    pushLog("Initialized ImGui + GLFW + OpenGL UI");
    return true;
}

void App::run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderFrame();

        ImGui::Render();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.06f, 0.06f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window_);
    }
}

void App::renderFrame() {
    renderMainMenu();
    renderDockspace();
    renderLeftPanel();
    renderRightPanel();
    renderBottomPanel();
    refreshMetrics();
}

void App::renderDockspace() {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("DockRoot", nullptr, flags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspaceId = ImGui::GetID("VSMDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    if (!dockLayoutInitialized_) {
        setupDefaultDockLayout();
    }

    ImGui::End();
}

void App::setupDefaultDockLayout() {
    ImGuiID dockspaceId = ImGui::GetID("VSMDockspace");
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    ImGuiID center = dockspaceId;

    ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.32f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.35f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.35f, &bottom, &center);

    ImGui::DockBuilderDockWindow("Queue", left);
    ImGui::DockBuilderDockWindow("Controls", right);
    ImGui::DockBuilderDockWindow("Logs & Metrics", bottom);

    ImGui::DockBuilderFinish(dockspaceId);
    dockLayoutInitialized_ = true;
}

void App::renderMainMenu() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Clear Queue")) {
                std::scoped_lock lock(queueMutex_);
                queue_.clear();
                selectedQueueIndex_ = -1;
            }
            if (ImGui::MenuItem("Reset Layout")) {
                dockLayoutInitialized_ = false;
            }
            if (ImGui::MenuItem("Exit")) {
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("About")) {
            showAbout_ = true;
        }
        ImGui::EndMainMenuBar();
    }

    if (showAbout_) {
        ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("About Monosect", &showAbout_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
        if (fullLogoTexture_.valid()) {
            const float maxWidth = std::min(460.0f, ImGui::GetContentRegionAvail().x);
            const float ratio = static_cast<float>(fullLogoTexture_.height) / static_cast<float>(fullLogoTexture_.width);
            const float centerOffset = (ImGui::GetContentRegionAvail().x - maxWidth) * 0.5f;
            if (centerOffset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerOffset);
            ImGui::Image(static_cast<ImTextureID>(fullLogoTexture_.id), ImVec2(maxWidth, maxWidth * ratio));
        } else {
            ImGui::TextUnformatted("MONOSECT");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Monosect");
        ImGui::TextUnformatted("High-performance lossless video split & merge utility");
        ImGui::Spacing();
        ImGui::TextDisabled("Built with ImGui + GLFW + OpenGL | FFmpeg engine");
        ImGui::TextDisabled("(c) Monosect");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            showAbout_ = false;
        }
        ImGui::End();
    }
}

void App::renderLeftPanel() {
    ImGui::Begin("Queue");

    if (ImGui::Button("Add Files")) {
        const auto files = openFilesDialog();
        for (const auto& file : files) {
            addPath(file);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
        removeSelectedFromQueue();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        std::scoped_lock lock(queueMutex_);
        queue_.clear();
        selectedQueueIndex_ = -1;
    }
    ImGui::SameLine();
    if (ffmpegBootstrapRunning_.load()) {
        ImGui::BeginDisabled();
        ImGui::Button("Start Split");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(Setting up FFmpeg...)");
    } else if (!ffmpegReady_.load()) {
        ImGui::BeginDisabled();
        ImGui::Button("Start Split");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(FFmpeg unavailable)");
    } else {
        if (ImGui::Button("Start Split")) {
            startSplitJobs();
        }
    }

    ImGui::Separator();
    const auto queueSnapshot = snapshotQueue();
    ImGui::Text("Items: %d", static_cast<int>(queueSnapshot.size()));
    ImGui::BeginChild("QueueList", ImVec2(0.0f, -1.0f), true);
    if (queueSnapshot.empty()) {
        ImGui::TextDisabled("Drop video files here or click 'Add Files'");
    }
    for (std::size_t i = 0; i < queueSnapshot.size(); ++i) {
        const bool selected = (selectedQueueIndex_ == static_cast<int>(i));
        const std::string label = std::to_string(i + 1) + ": " + queueSnapshot[i].filename().string();
        if (ImGui::Selectable(label.c_str(), selected)) {
            selectedQueueIndex_ = static_cast<int>(i);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", queueSnapshot[i].string().c_str());
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void App::renderRightPanel() {
    ImGui::Begin("Controls");

    ImGui::SeparatorText("Split Configuration");

    static const char* modes[] = {
        "Exact Time",
        "Exact Size",
        "Half By Time",
        "Half By Size",
        "Torrent 700MB",
        "Torrent 1GB",
        "Torrent 2GB",
        "Torrent 4.7GB",
        "Torrent 8.5GB"
    };
    ImGui::Combo("Split Mode", &splitModeIndex_, modes, IM_ARRAYSIZE(modes));

    static const char* precisionModes[] = {
        "Strict Lossless",
        "Exact Frame Precision",
        "Hybrid"
    };
    ImGui::Combo("Precision", &precisionIndex_, precisionModes, IM_ARRAYSIZE(precisionModes));

    ImGui::InputText("Time(sec)/Size(MB)", splitInput_, sizeof(splitInput_));

    std::vector<const char*> encoderItems;
    encoderItems.reserve(encoderOptions_.size());
    for (const auto& item : encoderOptions_) {
        encoderItems.push_back(item.c_str());
    }
    if (!encoderItems.empty()) {
        ImGui::Combo("Encoder", &encoderIndex_, encoderItems.data(), static_cast<int>(encoderItems.size()));
    }

    ImGui::Checkbox("Unrestricted Mode", &unrestrictedMode_);

    if (const auto selected = selectedQueuePath(); selected.has_value()) {
        ImGui::SeparatorText("Preview");
        try {
            const auto info = mediaProbe_.probe(*selected);
            ImGui::TextWrapped("File: %s", selected->string().c_str());
            ImGui::Text("Duration: %.2f s", info.durationSeconds);
            ImGui::Text("Video: %s", info.videoCodec.empty() ? "unknown" : info.videoCodec.c_str());
            ImGui::Text("Resolution: %d x %d", info.width, info.height);
            ImGui::Text("FPS: %.2f", info.fps);
            ImGui::Text("Bitrate: %.2f Mbps", static_cast<double>(info.bitRate) / 1000000.0);
            ImGui::Text("Audio tracks: %d", static_cast<int>(info.audioTracks.size()));
            ImGui::Text("Subtitle tracks: %d", static_cast<int>(info.subtitleTracks.size()));
        } catch (...) {
            ImGui::TextDisabled("Preview failed for selected file");
        }
    }

    ImGui::SeparatorText("Watch Folder");
    ImGui::Checkbox("Enable", &watchFolderEnabled_);
    ImGui::InputText("Watch Path", watchFolderInput_, sizeof(watchFolderInput_));

    if (ImGui::Button("Apply Watch Folder")) {
        if (watchFolderEnabled_ && std::strlen(watchFolderInput_) > 0) {
            watchFolder_.start(watchFolderInput_, [this](const std::filesystem::path& path) {
                threadPool_.enqueue([this, path]() { addPath(path); });
            });
            pushLog(std::string("Watching folder: ") + watchFolderInput_);
        } else {
            watchFolder_.stop();
            pushLog("Watch folder stopped");
        }
    }

    ImGui::End();
}

void App::renderBottomPanel() {
    ImGui::Begin("Logs & Metrics");

    // --- Metrics section ---
    ImGui::SeparatorText("System Metrics");

    ImGui::Text("CPU: %.1f%%", latestMetrics_.cpuUsagePercent);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Text("GPU: %.1f%%", latestMetrics_.gpuUsagePercent);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Text("Throughput: %.2f MB/s", latestMetrics_.throughputMBps);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Text("ETA: %.1f s", latestMetrics_.etaSeconds);

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float plotWidth = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
    ImGui::PlotLines("##Throughput", throughputPlot_.data(), static_cast<int>(throughputPlot_.size()), plotCursor_, "Throughput MB/s", 0.0f, 2048.0f, ImVec2(plotWidth, 60));
    ImGui::SameLine();
    ImGui::PlotLines("##CPU", cpuPlot_.data(), static_cast<int>(cpuPlot_.size()), plotCursor_, "CPU %", 0.0f, 100.0f, ImVec2(plotWidth, 60));
    ImGui::SameLine();
    ImGui::PlotLines("##GPU", gpuPlot_.data(), static_cast<int>(gpuPlot_.size()), plotCursor_, "GPU %", 0.0f, 100.0f, ImVec2(plotWidth, 60));

    // --- Log output section ---
    ImGui::SeparatorText("Log Output");
    ImGui::BeginChild("LogView", ImVec2(0.0f, -1.0f), true);
    {
        std::scoped_lock lock(logsMutex_);
        for (const auto& line : logs_) {
            ImGui::TextWrapped("%s", line.c_str());
        }
        if (!logs_.empty()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void App::pushLog(const std::string& line) {
    std::scoped_lock lock(logsMutex_);
    logs_.push_back(line);
    if (logs_.size() > 1000) {
        logs_.erase(logs_.begin(), logs_.begin() + 200);
    }
}

void App::addPath(const std::filesystem::path& path) {
    if (!isSafePath(path) || !std::filesystem::exists(path)) {
        pushLog("Rejected unsafe or missing path: " + path.string());
        return;
    }

    if (std::filesystem::is_directory(path)) {
        std::vector<std::filesystem::path> discovered;
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && isSupportedMediaExtension(entry.path())) {
                    discovered.push_back(entry.path());
                }
            }
        } catch (const std::exception& e) {
            pushLog(std::string("Directory scan warning: ") + e.what());
        }
        if (!discovered.empty()) {
            std::scoped_lock lock(queueMutex_);
            queue_.insert(queue_.end(), discovered.begin(), discovered.end());
        }
        pushLog("Added folder entries: " + path.string());
        return;
    }

    if (isSupportedMediaExtension(path)) {
        {
            std::scoped_lock lock(queueMutex_);
            queue_.push_back(path);
        }
        try {
            const auto info = mediaProbe_.probe(path);
            std::ostringstream line;
            line << "Queued " << path.string() << " [" << info.width << "x" << info.height << " "
                 << info.fps << "fps " << (info.bitRate / 1000) << "kbps " << info.durationSeconds << "s]";
            pushLog(line.str());
        } catch (...) {
            pushLog("Queued file (probe failed): " + path.string());
        }
    }
}

SplitRequest App::makeRequest(const std::filesystem::path& path) const {
    SplitRequest request;
    request.inputPath = path;
    request.outputDirectory = path.parent_path();
    request.unrestrictedMode = unrestrictedMode_;
    request.useHardwareAcceleration = !encoderOptions_.empty() && encoderOptions_[encoderIndex_] != "copy";

    if (precisionIndex_ == 0) request.precisionMode = PrecisionMode::StrictLossless;
    else if (precisionIndex_ == 1) request.precisionMode = PrecisionMode::ExactFrameBoundary;
    else request.precisionMode = PrecisionMode::Hybrid;

    const double numeric = std::atof(splitInput_);
    const double safeNumeric = (std::isfinite(numeric) && numeric >= 0.0) ? numeric : 0.0;

    switch (splitModeIndex_) {
    case 0:
        request.splitMode = SplitMode::ExactTime;
        request.splitTimeSeconds = safeNumeric;
        break;
    case 1:
        request.splitMode = SplitMode::ExactSize;
        request.splitSizeBytes = static_cast<int64_t>(safeNumeric * 1024.0 * 1024.0);
        break;
    case 2:
        request.splitMode = SplitMode::HalfByTime;
        break;
    case 3:
        request.splitMode = SplitMode::HalfBySize;
        request.splitSizeBytes = 0;
        break;
    case 4:
        request.splitMode = SplitMode::TorrentPreset;
        request.splitSizeBytes = 700LL * 1024LL * 1024LL;
        break;
    case 5:
        request.splitMode = SplitMode::TorrentPreset;
        request.splitSizeBytes = 1024LL * 1024LL * 1024LL;
        break;
    case 6:
        request.splitMode = SplitMode::TorrentPreset;
        request.splitSizeBytes = 2LL * 1024LL * 1024LL * 1024LL;
        break;
    case 7:
        request.splitMode = SplitMode::TorrentPreset;
        request.splitSizeBytes = static_cast<int64_t>(4.7 * 1024.0 * 1024.0 * 1024.0);
        break;
    default:
        request.splitMode = SplitMode::TorrentPreset;
        request.splitSizeBytes = static_cast<int64_t>(8.5 * 1024.0 * 1024.0 * 1024.0);
        break;
    }

    return request;
}

void App::startSplitJobs() {
    std::vector<std::filesystem::path> queueCopy;
    {
        std::scoped_lock lock(queueMutex_);
        if (queue_.empty()) {
            pushLog("Queue is empty");
            return;
        }
        queueCopy = queue_;
    }
    for (const auto& item : queueCopy) {
        threadPool_.enqueue([this, item]() {
            const auto start = std::chrono::steady_clock::now();
            const auto before = std::filesystem::exists(item) ? std::filesystem::file_size(item) : 0;

            const auto request = makeRequest(item);
            const auto result = splitEngine_.split(request);

            const auto end = std::chrono::steady_clock::now();
            const auto elapsedSeconds = std::chrono::duration<double>(end - start).count();
            if (elapsedSeconds > 0.0) {
                throughputMBps_.store((before / 1024.0 / 1024.0) / elapsedSeconds);
            }

            if (result.success) {
                pushLog("Split complete: " + item.string());
            } else {
                pushLog("Split failed: " + item.string() + " | " + result.message);
            }
        });
    }
}

void App::refreshMetrics() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMetricsTick_).count() < 300) {
        return;
    }
    lastMetricsTick_ = now;

    latestMetrics_ = systemMonitor_.sample(throughputMBps_.load(), etaSeconds_.load());

    throughputPlot_[plotCursor_] = static_cast<float>(latestMetrics_.throughputMBps);
    cpuPlot_[plotCursor_] = static_cast<float>(latestMetrics_.cpuUsagePercent);
    gpuPlot_[plotCursor_] = static_cast<float>(latestMetrics_.gpuUsagePercent);
    plotCursor_ = (plotCursor_ + 1) % static_cast<int>(throughputPlot_.size());
}

void App::dropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || paths == nullptr) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        if (paths[i] != nullptr) {
            app->addPath(paths[i]);
        }
    }
}

void App::removeSelectedFromQueue() {
    std::scoped_lock lock(queueMutex_);
    if (selectedQueueIndex_ < 0 || selectedQueueIndex_ >= static_cast<int>(queue_.size())) {
        return;
    }
    queue_.erase(queue_.begin() + selectedQueueIndex_);
    if (selectedQueueIndex_ >= static_cast<int>(queue_.size())) {
        selectedQueueIndex_ = static_cast<int>(queue_.size()) - 1;
    }
}

std::optional<std::filesystem::path> App::selectedQueuePath() const {
    std::scoped_lock lock(queueMutex_);
    if (selectedQueueIndex_ < 0 || selectedQueueIndex_ >= static_cast<int>(queue_.size())) {
        return std::nullopt;
    }
    return queue_[selectedQueueIndex_];
}

std::vector<std::filesystem::path> App::snapshotQueue() const {
    std::scoped_lock lock(queueMutex_);
    return queue_;
}

std::vector<std::filesystem::path> App::openFilesDialog() const {
    std::vector<std::filesystem::path> selected;

    wchar_t buffer[65536] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrFilter = L"Video Files\0*.mp4;*.mkv;*.mov;*.avi;*.flv;*.webm\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT;

    if (!GetOpenFileNameW(&ofn)) {
        return selected;
    }

    const wchar_t* ptr = buffer;
    const std::filesystem::path first(ptr);
    ptr += std::wcslen(ptr) + 1;

    if (*ptr == '\0') {
        selected.push_back(first);
        return selected;
    }

    const std::filesystem::path directory = first;
    while (*ptr != '\0') {
        std::filesystem::path filename(ptr);
        selected.emplace_back(directory / filename);
        ptr += std::wcslen(ptr) + 1;
    }

    return selected;
}

}