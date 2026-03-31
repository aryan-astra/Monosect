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
#include <cstdint>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iterator>
#include <limits>
#include <sstream>
#include <cwchar>
#include <system_error>

namespace vsm {

namespace {

#ifndef MONOSECT_VERSION_STR
#define MONOSECT_VERSION_STR "dev"
#endif

ImGuiStyle gBaseStyle;
bool gHasBaseStyle = false;
float gAppliedUiScale = 1.0f;

void renderHelpMarker(const char* description) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(description);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void renderFieldLabel(const char* label, const char* description) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (description != nullptr && *description != '\0') {
        renderHelpMarker(description);
    }
}

} // namespace

App::App()
    : threadPool_(std::max(1u, std::thread::hardware_concurrency())),
      maxWorkerLimit_(std::max(1u, std::thread::hardware_concurrency())),
      hardwareWorkerCount_(std::max(1u, std::thread::hardware_concurrency())) {
    tunedWorkerLimit_.store(maxWorkerLimit_);
    tunedMaxWorkers_.store(maxWorkerLimit_);
    autoTunerEnabled_.store(autoTunerEnabledUi_);
}

App::~App() {
    if (autoTuneThread_.joinable()) {
        autoTuneThread_.request_stop();
        autoTuneThread_.join();
    }
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

    const std::string windowTitle = std::string("Monosect v") + MONOSECT_VERSION_STR;
    window_ = glfwCreateWindow(1600, 960, windowTitle.c_str(), nullptr, nullptr);
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

    gBaseStyle = style;
    gHasBaseStyle = true;

    // DPI-aware font loading (falls back to built-in font if unavailable)
    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", std::round(16.0f * xscale));
    updateResponsiveScale();

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
    {
        std::scoped_lock lock(encoderMutex_);
        encoderOptions_.push_back("copy"); // default until detection finishes
        encoderIndex_ = 0;
    }
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
            std::scoped_lock lock(encoderMutex_);
            encoderOptions_.clear();
            if (enc.h264Nvenc) encoderOptions_.push_back("h264_nvenc");
            if (enc.hevcNvenc) encoderOptions_.push_back("hevc_nvenc");
            if (enc.h264Qsv) encoderOptions_.push_back("h264_qsv");
            if (enc.h264Amf) encoderOptions_.push_back("h264_amf");
            encoderOptions_.push_back("copy");
            encoderIndex_ = static_cast<int>(encoderOptions_.size()) - 1;
        }

        ffmpegReady_.store(true);
        ffmpegBootstrapRunning_.store(false);
        pushLog("FFmpeg ready.");
    });

    autoTuneThread_ = std::jthread([this](std::stop_token token) {
        runAutoTuner(token);
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
    updateResponsiveScale();
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
        ImGui::TextDisabled("Version: v%s", MONOSECT_VERSION_STR);
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
    if (splitInProgress_.load()) {
        ImGui::BeginDisabled();
        ImGui::Button("Start Split");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(Split in progress...)");
    } else if (ffmpegBootstrapRunning_.load()) {
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
    const auto queueSnapshot = snapshotQueueEntries();
    ImGui::Text("Items: %d", static_cast<int>(queueSnapshot.size()));
    ImGui::BeginChild("QueueList", ImVec2(0.0f, -1.0f), true);
    if (queueSnapshot.empty()) {
        ImGui::TextDisabled("Drop video files here or click 'Add Files'");
    }
    for (std::size_t i = 0; i < queueSnapshot.size(); ++i) {
        const bool selected = (selectedQueueIndex_ == static_cast<int>(i));
        const int pct = static_cast<int>(std::round(std::clamp(queueSnapshot[i].progress01, 0.0, 1.0) * 100.0));
        std::string label = std::to_string(i + 1) + ": " + queueSnapshot[i].path.filename().string() + " [" + std::to_string(pct) + "%]";
        if (queueSnapshot[i].hasEstimate && queueSnapshot[i].estimatedChunks > 0) {
            label += " ~" + std::to_string(queueSnapshot[i].estimatedChunks) + " chunks";
        }
        if (ImGui::Selectable(label.c_str(), selected)) {
            selectedQueueIndex_ = static_cast<int>(i);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(queueSnapshot[i].path.string().c_str());
            ImGui::Separator();
            ImGui::TextWrapped("Status: %s", queueSnapshot[i].status.c_str());
            if (queueSnapshot[i].hasEstimate && queueSnapshot[i].estimatedChunks > 0) {
                ImGui::Text("Dry-run estimate: %d chunks", queueSnapshot[i].estimatedChunks);
            }
            ImGui::EndTooltip();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void App::renderRightPanel() {
    ImGui::Begin("Controls");

    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const float compactModeThreshold = 430.0f;
    const bool compactMode = panelWidth < compactModeThreshold;
    const float labelWidth = compactMode ? panelWidth : std::clamp(panelWidth * 0.42f, 120.0f, 240.0f);
    const float inputWidth = compactMode ? panelWidth : std::max(100.0f, panelWidth - labelWidth - 8.0f);

    auto beginFieldLayout = [&](const char* label, const char* helpText) {
        if (compactMode) {
            renderFieldLabel(label, helpText);
            ImGui::SetNextItemWidth(-FLT_MIN);
            return;
        }
        renderFieldLabel(label, helpText);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, labelWidth - ImGui::GetCursorPosX()));
        ImGui::SetNextItemWidth(inputWidth);
    };

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
    beginFieldLayout("Split Mode", "How to split files.\nBest default: Exact Size for predictable file chunking. Use Half modes for quick 2-part splits.");
    ImGui::Combo("##SplitMode", &splitModeIndex_, modes, IM_ARRAYSIZE(modes));

    static const char* precisionModes[] = {
        "Strict Lossless",
        "Exact Frame Precision",
        "Hybrid"
    };
    beginFieldLayout("Precision", "Split precision.\nBest option: Strict Lossless for zero quality loss and fastest processing.");
    ImGui::Combo("##Precision", &precisionIndex_, precisionModes, IM_ARRAYSIZE(precisionModes));

    beginFieldLayout("Time/Size (MB)", "For Exact Time enter seconds. For Exact Size enter MB.\nExample: 1536 for ~1.5 GB.");
    ImGui::InputText("##TimeOrSize", splitInput_, sizeof(splitInput_));

    beginFieldLayout("Min File Size (MB)", "Only split files >= this size in size-based modes.\nBest default: set slightly above target chunk size.");
    ImGui::InputText("##MinFileSize", minFileSizeInput_, sizeof(minFileSizeInput_));

    std::vector<std::string> encoderItemsOwned;
    int encoderIndexLocal = 0;
    {
        std::scoped_lock lock(encoderMutex_);
        encoderItemsOwned = encoderOptions_;
        if (!encoderItemsOwned.empty()) {
            encoderIndex_ = std::clamp(encoderIndex_, 0, static_cast<int>(encoderItemsOwned.size()) - 1);
            encoderIndexLocal = encoderIndex_;
        }
    }
    std::vector<const char*> encoderItems;
    encoderItems.reserve(encoderItemsOwned.size());
    for (const auto& item : encoderItemsOwned) {
        encoderItems.push_back(item.c_str());
    }
    if (!encoderItems.empty()) {
        beginFieldLayout("Encoder", "Encoder preference.\nFor true lossless split, keep stream copy behavior (no re-encode).");
        const bool strictLossless = precisionIndex_ == 0;
        if (strictLossless) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Combo("##Encoder", &encoderIndexLocal, encoderItems.data(), static_cast<int>(encoderItems.size()))) {
            std::scoped_lock lock(encoderMutex_);
            encoderIndex_ = std::clamp(encoderIndexLocal, 0, static_cast<int>(encoderItems.size()) - 1);
        }
        if (strictLossless) {
            ImGui::EndDisabled();
            renderHelpMarker("Strict Lossless mode always uses stream copy (-c copy) for zero quality loss.");
        }
    }

    beginFieldLayout("Unrestricted Mode", "When OFF, files smaller than target size are skipped in size-based modes.\nBest default: OFF.");
    ImGui::Checkbox("##UnrestrictedMode", &unrestrictedMode_);

    beginFieldLayout("Auto-Tuner", "Dynamically adjusts active worker jobs and ffmpeg threading based on CPU pressure and throughput.");
    if (ImGui::Checkbox("##AutoTuner", &autoTunerEnabledUi_)) {
        autoTunerEnabled_.store(autoTunerEnabledUi_);
    }
    beginFieldLayout("Max Workers", "Upper bound for concurrently active split jobs.");
    ImGui::SetNextItemWidth(inputWidth);
    ImGui::SliderInt("##MaxWorkers", &maxWorkerLimit_, 1, std::max(1, hardwareWorkerCount_));
    renderHelpMarker("Tuned active jobs: current / cap");
    ImGui::SameLine();
    ImGui::Text("%d / %d", activeSplitJobs_.load(), tunedWorkerLimit_.load());

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
    beginFieldLayout("Enable", "Automatically enqueue new media files from the watch path.");
    ImGui::Checkbox("##WatchEnable", &watchFolderEnabled_);
    beginFieldLayout("Watch Path", "Folder path to monitor for incoming files.");
    ImGui::InputText("##WatchPath", watchFolderInput_, sizeof(watchFolderInput_));

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
    renderHelpMarker("Apply current watch-folder settings.");

    if (ImGui::Button("Dry-run Estimate")) {
        runDryRunEstimate();
    }
    renderHelpMarker("Computes estimated chunk count for queued files without writing output files.");

    ImGui::End();
}

void App::renderBottomPanel() {
    ImGui::Begin("Logs & Metrics");

    ImGui::SeparatorText("Split Progress");
    const double overallProgress = overallSplitProgress01();
    const int completed = completedSplitJobs_.load();
    const int total = totalSplitJobs_.load();
    const std::string progressLabel = std::to_string(static_cast<int>(std::round(overallProgress * 100.0))) + "%";
    ImGui::ProgressBar(static_cast<float>(overallProgress), ImVec2(-1.0f, 0.0f), progressLabel.c_str());
    ImGui::Text("Jobs: %d / %d", completed, total);
    const auto [currentPath, currentStatus] = currentSplitStatusSnapshot();
    if (!currentPath.empty()) {
        ImGui::TextWrapped("Current file: %s", currentPath.filename().string().c_str());
    }
    if (!currentStatus.empty()) {
        ImGui::TextWrapped("Status: %s", currentStatus.c_str());
    }
    if (splitInProgress_.load()) {
        if (splitPauseRequested_.load()) {
            if (ImGui::Button("Resume")) {
                splitPauseRequested_.store(false);
                splitControlCv_.notify_all();
                pushLog("Split resumed");
            }
        } else {
            if (ImGui::Button("Pause")) {
                splitPauseRequested_.store(true);
                pushLog("Pause requested");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            splitCancelRequested_.store(true);
            splitPauseRequested_.store(false);
            splitControlCv_.notify_all();
            splitConcurrencyCv_.notify_all();
            pushLog("Cancel requested; stopping active split");
        }
    }

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

void App::updateResponsiveScale() {
    if (window_ == nullptr || !gHasBaseStyle) {
        return;
    }

    float contentScaleX = 1.0f;
    float contentScaleY = 1.0f;
    glfwGetWindowContentScale(window_, &contentScaleX, &contentScaleY);
    const float dpiScale = std::max(contentScaleX, contentScaleY);

    int width = 0;
    int height = 0;
    glfwGetWindowSize(window_, &width, &height);
    const float windowFactor = std::clamp(static_cast<float>(std::min(width, height)) / 900.0f, 0.88f, 1.18f);
    const float nextScale = std::clamp(dpiScale * windowFactor, 0.85f, 1.45f);

    if (std::fabs(nextScale - gAppliedUiScale) < 0.01f) {
        return;
    }

    auto& style = ImGui::GetStyle();
    style = gBaseStyle;
    style.ScaleAllSizes(nextScale);
    style.WindowPadding = ImVec2(std::max(8.0f, style.WindowPadding.x), std::max(8.0f, style.WindowPadding.y));
    style.ItemSpacing = ImVec2(std::max(5.0f, style.ItemSpacing.x), std::max(4.0f, style.ItemSpacing.y));
    style.FramePadding = ImVec2(std::max(6.0f, style.FramePadding.x), std::max(4.0f, style.FramePadding.y));

    ImGui::GetIO().FontGlobalScale = std::clamp(nextScale, 0.9f, 1.35f);
    gAppliedUiScale = nextScale;
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
            for (const auto& file : discovered) {
                addFileToQueue(file);
            }
        }
        pushLog("Added folder entries: " + path.string());
        return;
    }

    if (isSupportedMediaExtension(path)) {
        addFileToQueue(path);
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

void App::addFileToQueue(const std::filesystem::path& path) {
    std::scoped_lock lock(queueMutex_);
    QueueEntry entry;
    entry.id = nextQueueEntryId_++;
    entry.path = path;
    queue_.push_back(std::move(entry));
}

SplitRequest App::makeRequest(const std::filesystem::path& path) const {
    SplitRequest request;
    request.inputPath = path;
    request.outputDirectory = path.parent_path();
    request.unrestrictedMode = unrestrictedMode_;
    const auto encoderValue = selectedEncoderValue();
    const bool encoderSelected = !encoderValue.empty() && encoderValue != "copy";
    request.useHardwareAcceleration = encoderSelected;

    if (precisionIndex_ == 0) request.precisionMode = PrecisionMode::StrictLossless;
    else if (precisionIndex_ == 1) request.precisionMode = PrecisionMode::ExactFrameBoundary;
    else request.precisionMode = PrecisionMode::Hybrid;

    if (request.precisionMode == PrecisionMode::StrictLossless) {
        request.codecOverride = "copy";
        request.fastSeekEnabled = true;
        request.useHardwareAcceleration = false;
    } else if (encoderSelected) {
        request.codecOverride = encoderValue;
        request.fastSeekEnabled = false;
    } else {
        request.codecOverride = "copy";
        request.fastSeekEnabled = true;
    }

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

std::string App::selectedEncoderValue() const {
    std::scoped_lock lock(encoderMutex_);
    if (encoderOptions_.empty()) {
        return "copy";
    }
    const int idx = std::clamp(encoderIndex_, 0, static_cast<int>(encoderOptions_.size()) - 1);
    return encoderOptions_[idx];
}

void App::startSplitJobs() {
    std::vector<QueueEntry> queueCopy;
    {
        std::scoped_lock lock(queueMutex_);
        if (queue_.empty()) {
            pushLog("Queue is empty");
            return;
        }
        queueCopy = queue_;
    }
    minFileSizeBytes_.store(parseMegabytesToBytes(minFileSizeInput_));
    currentTargetBytes_.store(parseMegabytesToBytes(splitInput_));
    tunedMaxWorkers_.store(std::max(1, maxWorkerLimit_));
    tunedWorkerLimit_.store(std::max(1, std::min(maxWorkerLimit_, hardwareWorkerCount_)));
    activeSplitJobs_.store(0);
    splitConcurrencyCv_.notify_all();
    splitInProgress_.store(true);
    splitPauseRequested_.store(false);
    splitCancelRequested_.store(false);
    totalSplitJobs_.store(static_cast<int>(queueCopy.size()));
    completedSplitJobs_.store(0);
    currentJobProgress01_.store(0.0);
    {
        std::scoped_lock statusLock(splitStatusMutex_);
        currentJobPath_.clear();
        currentJobStatus_.assign("Queued jobs");
    }
    {
        std::scoped_lock lock(queueMutex_);
        for (auto& entry : queue_) {
            entry.progress01 = 0.0;
            entry.status = "Queued";
            entry.hasEstimate = false;
            entry.estimatedChunks = 0;
        }
    }

    struct PendingSplitJob {
        QueueEntry queueEntry;
        SplitRequest baseRequest;
    };

    std::vector<PendingSplitJob> jobs;
    jobs.reserve(queueCopy.size());
    for (const auto& item : queueCopy) {
        jobs.push_back(PendingSplitJob{ item, makeRequest(item.path) });
    }

    for (const auto& job : jobs) {
        threadPool_.enqueue([this, job]() {
            const auto& item = job.queueEntry;
            if (!acquireConcurrencySlot()) {
                updateQueueEntry(item.id, 1.0, "Canceled", true);
                setCurrentSplitStatus(item.path, 1.0, "Canceled");
                const int doneCanceled = completedSplitJobs_.fetch_add(1) + 1;
                if (doneCanceled >= totalSplitJobs_.load()) {
                    splitInProgress_.store(false);
                    currentJobProgress01_.store(1.0);
                }
                return;
            }

            if (!waitWhilePausedOrCanceled()) {
                updateQueueEntry(item.id, 1.0, "Canceled", true);
                setCurrentSplitStatus(item.path, 1.0, "Canceled");
                const int doneCanceled = completedSplitJobs_.fetch_add(1) + 1;
                if (doneCanceled >= totalSplitJobs_.load()) {
                    splitInProgress_.store(false);
                    currentJobProgress01_.store(1.0);
                }
                releaseConcurrencySlot();
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            std::error_code sizeError;
            const auto before = std::filesystem::exists(item.path) ? std::filesystem::file_size(item.path, sizeError) : 0;

            auto request = job.baseRequest;
            const int64_t minimumFileSizeBytes = minFileSizeBytes_.load();
            if (minimumFileSizeBytes > 0) {
                request.minimumFileSizeBytes = minimumFileSizeBytes;
            }
            request.desiredThreads = std::max(1, hardwareWorkerCount_ / std::max(1, tunedWorkerLimit_.load()));
            request.shouldPause = [this]() {
                return shouldPauseSplit();
            };
            request.shouldCancel = [this]() {
                return shouldCancelSplit();
            };
            request.onProgress = [this, item](double progress01, const std::string& status) {
                setCurrentSplitStatus(item.path, progress01, status);
                updateQueueEntry(item.id, progress01, status);
            };
            setCurrentSplitStatus(item.path, 0.0, "Queued");
            updateQueueEntry(item.id, 0.0, "Queued");
            const auto result = splitEngine_.split(request);

            const auto end = std::chrono::steady_clock::now();
            const auto elapsedSeconds = std::chrono::duration<double>(end - start).count();
            if (elapsedSeconds > 0.0) {
                throughputMBps_.store((before / 1024.0 / 1024.0) / elapsedSeconds);
            }

            if (!result.success && shouldCancelSplit()) {
                pushLog("Canceled: " + item.path.string());
            } else if (result.success) {
                if (result.outputSegments.empty()) {
                    pushLog("Skipped: " + item.path.string() + " | " + result.message);
                } else {
                    pushLog("Split complete: " + item.path.string());
                }
            } else {
                pushLog("Split failed: " + item.path.string() + " | " + result.message);
            }

            setCurrentSplitStatus(item.path, 1.0, result.success ? result.message : ("Failed: " + result.message));
            updateQueueEntry(item.id, 1.0, result.success ? result.message : ("Failed: " + result.message), true);
            const int done = completedSplitJobs_.fetch_add(1) + 1;
            if (done >= totalSplitJobs_.load()) {
                splitInProgress_.store(false);
                currentJobProgress01_.store(1.0);
            }
            releaseConcurrencySlot();
        });
    }
}

void App::runDryRunEstimate() {
    std::vector<QueueEntry> queueCopy;
    {
        std::scoped_lock lock(queueMutex_);
        if (queue_.empty()) {
            pushLog("Dry-run skipped: queue is empty");
            return;
        }
        queueCopy = queue_;
    }

    currentTargetBytes_.store(parseMegabytesToBytes(splitInput_));
    minFileSizeBytes_.store(parseMegabytesToBytes(minFileSizeInput_));
    const int64_t targetBytes = currentTargetBytes_.load();
    const int splitMode = splitModeIndex_;
    if (targetBytes <= 0 && splitModeIndex_ == 1) {
        pushLog("Dry-run skipped: invalid target size");
        return;
    }

    threadPool_.enqueue([this, queueCopy, targetBytes, splitMode]() {
        const int64_t minimumFileSizeBytes = minFileSizeBytes_.load();
        for (const auto& entry : queueCopy) {
            if (!std::filesystem::exists(entry.path)) {
                updateQueueEstimate(entry.id, 0, "Dry-run: file missing");
                continue;
            }

            std::error_code sizeError;
            const auto fileSize = static_cast<int64_t>(std::filesystem::file_size(entry.path, sizeError));
            if (sizeError) {
                updateQueueEstimate(entry.id, 0, "Dry-run: unable to read size");
                continue;
            }

            if (minimumFileSizeBytes > 0 && fileSize < minimumFileSizeBytes) {
                updateQueueEstimate(entry.id, 0, "Dry-run: below minimum file size");
                continue;
            }

            int64_t effectiveTarget = targetBytes;
            if (splitMode == 3) {
                effectiveTarget = std::max<int64_t>(1, fileSize / 2);
            } else if (splitMode == 4) {
                effectiveTarget = 700LL * 1024LL * 1024LL;
            } else if (splitMode == 5) {
                effectiveTarget = 1024LL * 1024LL * 1024LL;
            } else if (splitMode == 6) {
                effectiveTarget = 2LL * 1024LL * 1024LL * 1024LL;
            } else if (splitMode == 7) {
                effectiveTarget = static_cast<int64_t>(4.7 * 1024.0 * 1024.0 * 1024.0);
            } else if (splitMode == 8) {
                effectiveTarget = static_cast<int64_t>(8.5 * 1024.0 * 1024.0 * 1024.0);
            }

            if (effectiveTarget <= 0) {
                updateQueueEstimate(entry.id, 0, "Dry-run: unsupported mode");
                continue;
            }

            const int chunks = static_cast<int>((fileSize + effectiveTarget - 1) / effectiveTarget);
            updateQueueEstimate(entry.id, chunks, "Dry-run: ~" + std::to_string(chunks) + " chunks");
        }
        pushLog("Dry-run estimate complete");
    });
}

void App::refreshMetrics() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMetricsTick_).count() < 300) {
        return;
    }
    lastMetricsTick_ = now;

    latestMetrics_ = systemMonitor_.sample(throughputMBps_.load(), etaSeconds_.load());
    latestCpuUsage_.store(latestMetrics_.cpuUsagePercent);

    throughputPlot_[plotCursor_] = static_cast<float>(latestMetrics_.throughputMBps);
    cpuPlot_[plotCursor_] = static_cast<float>(latestMetrics_.cpuUsagePercent);
    gpuPlot_[plotCursor_] = static_cast<float>(latestMetrics_.gpuUsagePercent);
    plotCursor_ = (plotCursor_ + 1) % static_cast<int>(throughputPlot_.size());
}

void App::setCurrentSplitStatus(const std::filesystem::path& path, double progress01, const std::string& status) {
    currentJobProgress01_.store(std::clamp(progress01, 0.0, 1.0));
    std::scoped_lock lock(splitStatusMutex_);
    currentJobPath_ = path;
    currentJobStatus_ = status;
}

void App::updateQueueEntry(uint64_t id, double progress01, const std::string& status, bool clearEstimate) {
    std::scoped_lock lock(queueMutex_);
    for (auto& entry : queue_) {
        if (entry.id != id) {
            continue;
        }
        entry.progress01 = std::clamp(progress01, 0.0, 1.0);
        entry.status = status;
        if (clearEstimate) {
            entry.hasEstimate = false;
            entry.estimatedChunks = 0;
        }
        return;
    }
}

void App::updateQueueEstimate(uint64_t id, int estimatedChunks, const std::string& status) {
    std::scoped_lock lock(queueMutex_);
    for (auto& entry : queue_) {
        if (entry.id != id) {
            continue;
        }
        entry.hasEstimate = estimatedChunks > 0;
        entry.estimatedChunks = std::max(0, estimatedChunks);
        entry.status = status;
        return;
    }
}

bool App::waitWhilePausedOrCanceled() {
    if (splitCancelRequested_.load()) {
        return false;
    }
    std::unique_lock lock(splitControlMutex_);
    splitControlCv_.wait(lock, [this]() {
        return !splitPauseRequested_.load() || splitCancelRequested_.load();
    });
    return !splitCancelRequested_.load();
}

bool App::shouldPauseSplit() const {
    return splitPauseRequested_.load();
}

bool App::shouldCancelSplit() const {
    return splitCancelRequested_.load();
}

bool App::acquireConcurrencySlot() {
    std::unique_lock lock(splitConcurrencyMutex_);
    splitConcurrencyCv_.wait(lock, [this]() {
        return shouldCancelSplit() || activeSplitJobs_.load() < std::max(1, tunedWorkerLimit_.load());
    });
    if (shouldCancelSplit()) {
        return false;
    }
    activeSplitJobs_.fetch_add(1);
    return true;
}

void App::releaseConcurrencySlot() {
    activeSplitJobs_.fetch_sub(1);
    splitConcurrencyCv_.notify_all();
}

void App::runAutoTuner(std::stop_token token) {
    double previousThroughput = 0.0;
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        if (token.stop_requested()) {
            break;
        }
        const bool splitting = splitInProgress_.load();
        if (!splitting) {
            previousThroughput = 0.0;
            continue;
        }

        const int maxAllowed = std::max(1, tunedMaxWorkers_.load());
        if (!autoTunerEnabled_.load()) {
            if (tunedWorkerLimit_.load() != maxAllowed) {
                tunedWorkerLimit_.store(maxAllowed);
                splitConcurrencyCv_.notify_all();
            }
            previousThroughput = throughputMBps_.load();
            continue;
        }

        int currentLimit = std::clamp(tunedWorkerLimit_.load(), 1, maxAllowed);
        const double cpu = latestCpuUsage_.load();
        const double throughput = throughputMBps_.load();
        const double delta = throughput - previousThroughput;
        previousThroughput = throughput;

        if (cpu > 97.0) {
            currentLimit = std::max(1, currentLimit - 1);
        } else if (cpu < 72.0 && delta >= -0.01) {
            currentLimit = std::min(maxAllowed, currentLimit + 1);
        } else if (cpu > 88.0 && delta < -0.05) {
            currentLimit = std::max(1, currentLimit - 1);
        }

        tunedWorkerLimit_.store(currentLimit);
        splitConcurrencyCv_.notify_all();
    }
}

int64_t App::parseMegabytesToBytes(const char* text) const {
    if (text == nullptr || *text == '\0') {
        return 0;
    }
    const double value = std::atof(text);
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    const double bytes = value * 1024.0 * 1024.0;
    if (bytes >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(bytes);
}

std::pair<std::filesystem::path, std::string> App::currentSplitStatusSnapshot() const {
    std::scoped_lock lock(splitStatusMutex_);
    return { currentJobPath_, currentJobStatus_ };
}

double App::overallSplitProgress01() const {
    std::scoped_lock lock(queueMutex_);
    if (queue_.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const auto& entry : queue_) {
        sum += std::clamp(entry.progress01, 0.0, 1.0);
    }
    return std::clamp(sum / static_cast<double>(queue_.size()), 0.0, 1.0);
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
    return queue_[selectedQueueIndex_].path;
}

std::vector<App::QueueEntry> App::snapshotQueueEntries() const {
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
