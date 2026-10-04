// MonoSect — the application window (Windows only; see MainWindow.hpp).
#include "ui/MainWindow.hpp"

#include "core/Bytes.hpp"
#include "core/Utf8Path.hpp"
#include "engine/FFmpegBootstrap.hpp"
#include "engine/ProcessRunners.hpp"
#include "ui/Branding.hpp"
#include "ui/UiController.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace monosect::ui {

using namespace monosect::core;

class MainWindow;

namespace {

#ifndef MONOSECT_VERSION_STR
#define MONOSECT_VERSION_STR "dev"
#endif

MainWindow* g_window = nullptr;   // for the GLFW drop callback

/// A line the toolchain thread uses to report its own failure; drained by the UI
/// thread, so no string crosses the thread boundary outside the queue's mutex.
constexpr const char* kBootstrapFailureMark = "!error! ";


ImVec4 toneColor(StatusTone tone) {
    switch (tone) {
        case StatusTone::Good: return ImVec4(0.45f, 0.80f, 0.50f, 1.0f);
        case StatusTone::Warning: return ImVec4(0.90f, 0.75f, 0.35f, 1.0f);
        case StatusTone::Bad: return ImVec4(0.90f, 0.45f, 0.45f, 1.0f);
        case StatusTone::Working: return ImVec4(0.55f, 0.72f, 0.90f, 1.0f);
        case StatusTone::Neutral: break;
    }
    return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
}

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.PopupRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;
    style.WindowBorderSize = 1.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.09f, 0.96f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.07f, 0.07f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.07f, 0.07f, 0.07f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.07f, 0.07f, 0.07f, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.34f, 0.34f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.30f, 1.0f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.30f, 0.30f, 0.80f);
    c[ImGuiCol_PlotLines] = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
}

/// A "(?)" that opens the help topic describing this control.  The tooltip is the
/// topic's one-line summary; clicking it opens the topic in the help window.
/// Draws the "(?)" that belongs to a control.  The tooltip is the help topic's
/// one-line summary and clicking it opens the topic, so every control that has a
/// rule behind it can explain the rule in the same words the help window uses.
void helpMarker(const HelpTopic* topic, const std::function<void(HelpTopicId)>& open) {
    if (topic == nullptr) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(topic->title.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(topic->oneLine.c_str());
        ImGui::TextDisabled("Click to open the full topic.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked() && open) {
        open(topic->id);
    }
}

std::string genericFilePathDialog(const wchar_t* title, bool save, const wchar_t* filter) {
    std::vector<wchar_t> buffer(32768, L'\0');
    OPENFILENAMEW info{};
    info.lStructSize = sizeof(info);
    info.lpstrFile = buffer.data();
    info.nMaxFile = static_cast<DWORD>(buffer.size());
    info.lpstrTitle = title;
    info.lpstrFilter = filter;
    info.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                      : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER);
    const BOOL ok = save ? GetSaveFileNameW(&info) : GetOpenFileNameW(&info);
    if (!ok) {
        return {};
    }
    const int wideLength = static_cast<int>(std::wcslen(buffer.data()));
    if (wideLength <= 0) {
        return {};
    }
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wideLength, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wideLength, result.data(), utf8Length, nullptr, nullptr);
    return result;
}

std::string chooseFolderDialog(const wchar_t* title) {
    BROWSEINFOW info{};
    info.hwndOwner = GetActiveWindow();
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE items = SHBrowseForFolderW(&info);
    if (items == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListW(items, path) == TRUE;
    CoTaskMemFree(items);
    if (!ok) {
        return {};
    }
    const int wideLength = static_cast<int>(std::wcslen(path));
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, path, wideLength, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, wideLength, result.data(), utf8Length, nullptr, nullptr);
    return result;
}

std::vector<std::string> openFileDialogMulti(const wchar_t* title) {
    std::vector<std::string> paths;
    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW info{};
    info.lStructSize = sizeof(info);
    info.lpstrFile = buffer.data();
    info.nMaxFile = static_cast<DWORD>(buffer.size());
    info.lpstrTitle = title;
    info.lpstrFilter = L"Media files\0*.mkv;*.mp4;*.mov;*.m4v;*.ts;*.m2ts;*.webm;*.avi;*.flv;*.wmv;*.mpg;*.mpeg\0"
                       L"All files\0*.*\0";
    info.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&info)) {
        return paths;
    }
    // Multi-select returns "folder\0name1\0name2\0\0" or a single full path.
    const wchar_t* cursor = buffer.data();
    const std::wstring first(cursor);
    cursor += first.size() + 1;
    if (*cursor == L'\0') {
        const int length = static_cast<int>(first.size());
        const int utf8 = WideCharToMultiByte(CP_UTF8, 0, first.c_str(), length, nullptr, 0, nullptr, nullptr);
        std::string path(static_cast<std::size_t>(utf8), '\0');
        WideCharToMultiByte(CP_UTF8, 0, first.c_str(), length, path.data(), utf8, nullptr, nullptr);
        paths.push_back(std::move(path));
        return paths;
    }
    while (*cursor != L'\0') {
        const std::wstring name(cursor);
        std::wstring full = first + L"\\" + name;
        const int length = static_cast<int>(full.size());
        const int utf8 = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), length, nullptr, 0, nullptr, nullptr);
        std::string path(static_cast<std::size_t>(utf8), '\0');
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), length, path.data(), utf8, nullptr, nullptr);
        paths.push_back(std::move(path));
        cursor += name.size() + 1;
    }
    return paths;
}

} // namespace

/// The window itself.  Declared here rather than in the header because the rest of
/// the application only needs `runApplicationWindow`.
class MainWindow {
public:
    explicit MainWindow(std::string version) : version_(std::move(version)) {
        g_window = this;
    }

    ~MainWindow() {
        if (bootstrapThread_.joinable()) {
            bootstrapThread_.request_stop();
            bootstrapThread_.join();
        }
        if (controller_) {
            controller_->requestCancel();
            controller_->waitForJob();
        }
        controller_.reset();
        engine_.reset();
        prober_.reset();
        if (imguiReady_) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
        }
        glfwTerminate();
        g_window = nullptr;
    }

    bool initialize() {
        if (glfwInit() == 0) {
            return false;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        const std::string title = "MonoSect " + version_;
        window_ = glfwCreateWindow(1500, 900, title.c_str(), nullptr, nullptr);
        if (window_ == nullptr) {
            return false;
        }
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        applyTheme();
        float xscale = 1.0f;
        float yscale = 1.0f;
        glfwGetWindowContentScale(window_, &xscale, &yscale);
        if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", std::round(16.0f * xscale)) == nullptr) {
            io.Fonts->AddFontDefault();
        }
        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init("#version 330");
        imguiReady_ = true;

        glfwSetWindowUserPointer(window_, this);
        glfwSetDropCallback(window_, [](GLFWwindow*, int count, const char** paths) {
            if (g_window == nullptr) {
                return;
            }
            for (int index = 0; index < count; ++index) {
                if (paths[index] != nullptr) {
                    g_window->addDroppedPath(paths[index]);
                }
            }
        });

        executableDirectory_ = vsm::executableDirectory();
        std::snprintf(outputDirectoryBuffer_, sizeof(outputDirectoryBuffer_), "%s",
                      (executableDirectory_ / "output").string().c_str());
        std::snprintf(namingBuffer_, sizeof(namingBuffer_), "%s", "{name} - part {index:03}");
        std::snprintf(sizeValueBuffer_, sizeof(sizeValueBuffer_), "%s", "700");
        std::snprintf(durationValueBuffer_, sizeof(durationValueBuffer_), "%s", "00:10:00");
        std::snprintf(databaseBuffer_, sizeof(databaseBuffer_), "%s",
                      JobStore::defaultStore().path().string().c_str());
        startToolBootstrap();
        return true;
    }

    int run() {
        while (glfwWindowShouldClose(window_) == 0) {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            if (controller_) {
                controller_->poll();
                pumpBootstrapLog();
                sampleResources();
                handleShortcuts();
            }
            drawMenuBar();
            drawDockspace();
            drawStatusBar();
            drawHelpWindow();
            drawAboutWindow();

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window_);
        }

        // The window is closing: a running job is cancelled honestly (finished
        // parts stay on disk) and the worker is joined before the process exits.
        if (controller_) {
            controller_->requestCancel();
            controller_->waitForJob();
        }
        return 0;
    }

    void addDroppedPath(const std::string& path) {
        if (controller_) {
            controller_->addSource(path);
            controller_->probeSelected();
        }
    }

private:
    // ---- startup --------------------------------------------------------------------

    void startToolBootstrap() {
        bootstrapRunning_ = true;
        bootstrapThread_ = std::jthread([this](std::stop_token) {
            const bool ok = vsm::ensureFFmpeg(executableDirectory_, [this](const std::string& line) {
                std::lock_guard<std::mutex> lock(bootstrapMutex_);
                bootstrapLog_.push_back(line);
            });
            if (!ok) {
                std::lock_guard<std::mutex> lock(bootstrapMutex_);
                bootstrapLog_.push_back(kBootstrapFailureMark +
                                        std::string("FFmpeg and FFprobe could not be prepared. Place ffmpeg.exe "
                                                    "and ffprobe.exe next to MonoSect.exe, or point MonoSect at "
                                                    "an existing installation."));
                return;
            }
            ffmpegPath_ = (executableDirectory_ / "ffmpeg.exe").string();
            ffprobePath_ = (executableDirectory_ / "ffprobe.exe").string();
            {
                std::lock_guard<std::mutex> lock(bootstrapMutex_);
                bootstrapLog_.push_back("FFmpeg ready: " + ffmpegPath_);
            }
            toolsReady_ = true;
        });
    }

    /// The controller needs the tools and is therefore created on the UI thread
    /// once they exist.
    void createControllerIfReady() {
        if (controller_ || !toolsReady_) {
            return;
        }
        prober_ = std::make_unique<FFprobeService>(windowsProcessRunner(), ffprobePath_);
        engine_ = std::make_unique<EngineUiEngine>(*prober_, windowsProcessRunner(), ffmpegPath_);
        hardware_ = std::make_unique<BackgroundHardwareProbe>(windowsProcessRunner(), ffmpegPath_);
        controller_ = std::make_unique<UiController>(*engine_, hardware_.get());
        // Started once the window exists, not here: the probe runs real FFmpeg
        // processes - one `ffmpeg -encoders` plus one encode per listed candidate -
        // and doing that on the UI thread would freeze the window for as long as it
        // takes, which is exactly the number of seconds the user is waiting to find
        // out about.
        hardware_->startProbe();
        controller_->setDatabasePath(databaseBuffer_);
        applySettingsToController();
        controller_->setOutputDirectory(outputDirectoryBuffer_);
        controller_->refreshHistory();
    }

    void pumpBootstrapLog() {
        std::vector<std::string> drained;
        {
            std::lock_guard<std::mutex> lock(bootstrapMutex_);
            drained.swap(bootstrapLog_);
        }
        for (std::string& line : drained) {
            if (line.rfind(kBootstrapFailureMark, 0) == 0) {
                bootstrapError_ = line.substr(std::strlen(kBootstrapFailureMark));
                bootstrapRunning_ = false;
                continue;
            }
            logLines_.push_back(std::move(line));
        }
    }

    // ---- settings -------------------------------------------------------------------

    void applySettingsToController() {
        if (!controller_) {
            return;
        }
        controller_->setNamingPattern(namingBuffer_);
        controller_->setSizeLimitBytes(sizeLimitEnabled_ ? parseSizeBytes(sizeValueBuffer_, sizeUnitIndex_) : 0);
        if (durationLimitEnabled_) {
            controller_->setDurationLimit(parseDurationSeconds(durationValueBuffer_));
        } else {
            controller_->clearDurationLimit();
        }
        controller_->setPreservation(static_cast<PreservationMode>(preservationIndex_));
        controller_->setPrecision(static_cast<PrecisionPolicy>(precisionIndex_));
        controller_->constraint().maxParts = maxPartsEnabled_ ? std::optional<int>(maxParts_) : std::nullopt;
        VerificationPolicy policy;
        policy.enforceSizeLimit = true;         // not switchable: an oversized part is a defect
        policy.enforceDurationLimit = true;
        policy.checkStreamSignatures = verifyStreams_;
        policy.requireKeyframeStart = verifyKeyframeStart_;
        policy.decodeCheck = verifyDecode_;
        controller_->setVerificationPolicy(policy);
        controller_->setLowPriority(lowPriority_);
        controller_->setFfmpegThreads(threadOverride_ ? threads_ : 0);
        controller_->setGpuEnabled(gpuEnabled_);
        controller_->setHardwareDecodingEnabled(hardwareDecodingEnabled_);
    }

    static ByteCount parseSizeBytes(const char* text, int unitIndex) {
        const double value = std::strtod(text, nullptr);
        if (!(value > 0.0)) {
            return 0;
        }
        const double multiplier = unitIndex == 0 ? 1024.0 * 1024.0 * 1024.0
                                                 : unitIndex == 1 ? 1024.0 * 1024.0 : 1024.0;
        return static_cast<ByteCount>(value * multiplier);
    }

    static Rational parseDurationSeconds(const char* text) {
        int hours = 0;
        int minutes = 0;
        double seconds = 0.0;
        if (std::sscanf(text, "%d:%d:%lf", &hours, &minutes, &seconds) != 3) {
            if (std::sscanf(text, "%d:%lf", &minutes, &seconds) != 2) {
                seconds = std::strtod(text, nullptr);
                if (!(seconds > 0.0)) {
                    return Rational{0, 1};
                }
                return Rational{static_cast<std::int64_t>(seconds * 1000.0 + 0.5), 1000};
            }
        }
        const std::int64_t milliseconds =
            static_cast<std::int64_t>((hours * 3600.0 + minutes * 60.0 + seconds) * 1000.0 + 0.5);
        return Rational{milliseconds, 1000};
    }

    void addSourceFromDialog() {
        if (!controller_) {
            return;
        }
        const std::vector<std::string> paths = openFileDialogMulti(L"Add source files");
        for (const std::string& path : paths) {
            controller_->addSource(path);
        }
        if (!paths.empty()) {
            controller_->probeSelected();
        }
    }

    void handleShortcuts() {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) {
            return;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
            addSourceFromDialog();
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) {
            controller_->previewPlan(controller_->outputDirectory());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            controller_->start(controller_->outputDirectory());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            controller_->requestCancel();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
            controller_->showHelp(HelpTopicId::FirstRun);
        }
    }

    // ---- chrome ---------------------------------------------------------------------

    void drawMenuBar() {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Add source...", "Ctrl+O")) {
                    addSourceFromDialog();
                }
                if (ImGui::MenuItem("Choose output folder...")) {
                    const std::string folder = chooseFolderDialog(L"Output folder for the parts");
                    if (!folder.empty()) {
                        std::snprintf(outputDirectoryBuffer_, sizeof(outputDirectoryBuffer_), "%s", folder.c_str());
                        if (controller_) {
                            controller_->setOutputDirectory(folder);
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) {
                    glfwSetWindowShouldClose(window_, 1);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Job")) {
                const bool running = controller_ && controller_->jobRunning();
                if (ImGui::MenuItem("Read the selected source", nullptr, false, controller_ != nullptr && !running)) {
                    controller_->probeSelected();
                }
                if (ImGui::MenuItem("Preview plan", "Ctrl+Enter", false, controller_ != nullptr && !running)) {
                    controller_->previewPlan(controller_->outputDirectory());
                }
                if (ImGui::MenuItem("Start", "F5", false, controller_ != nullptr && !running)) {
                    controller_->start(controller_->outputDirectory());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Pause at the next part boundary", nullptr, false, running)) {
                    controller_->requestPause();
                }
                if (ImGui::MenuItem("Resume", nullptr, false, running)) {
                    controller_->requestResume();
                }
                if (ImGui::MenuItem("Cancel", "Esc", false, running)) {
                    controller_->requestCancel();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Resources", nullptr, &showResources_);
                ImGui::MenuItem("Job history", nullptr, &showHistory_);
                ImGui::MenuItem("Log", nullptr, &showLog_);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (controller_ != nullptr) {
                    if (ImGui::MenuItem("Overview", "F1")) {
                        controller_->showHelp(HelpTopicId::FirstRun);
                    }
                    if (ImGui::MenuItem("All topics...")) {
                        controller_->showHelp(HelpTopicId::SizeLimit);
                    }
                }
                ImGui::Separator();
                ImGui::MenuItem("About MonoSect", nullptr, &showAbout_);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
    }

    void drawDockspace() {
        // A fixed three-column layout: the source and its limits on the left, the
        // plan and the results in the middle, the machine and the history on the
        // right.  The log sits under the results.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float statusBarHeight = ImGui::GetFrameHeightWithSpacing() + 8.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
        ImGui::Begin("##workspace", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollbar);
        const float leftWidth = 420.0f;
        const float rightWidth = showResources_ || showHistory_ ? 340.0f : 0.0f;

        ImGui::BeginChild("##left", ImVec2(leftWidth, 0.0f), ImGuiChildFlags_Border);
        drawFirstRunPanel();
        drawSourcePanel();
        drawConstraintPanel();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##middle", ImVec2(ImGui::GetContentRegionAvail().x - rightWidth - 8.0f, 0.0f),
                          ImGuiChildFlags_Border);
        drawRunPanel();
        drawPlanPanel();
        drawResultsPanel();
        if (showLog_) {
            drawLogPanel();
        }
        ImGui::EndChild();

        if (rightWidth > 0.0f) {
            ImGui::SameLine();
            ImGui::BeginChild("##right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Border);
            if (showResources_) {
                drawResourcesPanel();
            }
            if (showHistory_) {
                drawHistoryPanel();
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    void drawStatusBar() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float height = ImGui::GetFrameHeightWithSpacing() + 8.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
        if (controller_) {
            drawStatusLine();
        } else {
            ImGui::TextUnformatted(bootstrapRunning_ ? "Preparing the FFmpeg toolchain..." : "MonoSect is starting.");
        }
        ImGui::End();
    }

    void drawStatusLine() {
        const JobStatusLine line = controller_->statusLine();
        ImGui::TextColored(toneColor(line.tone), "%s", line.headline.c_str());
        if (!line.detail.empty()) {
            ImGui::SameLine();
            ImGui::TextUnformatted(line.detail.c_str());
        }
        if (controller_->previewIsStale()) {
            ImGui::SameLine();
            ImGui::TextColored(toneColor(StatusTone::Warning), "(the plan is out of date)");
        }
    }

    // ---- panels ---------------------------------------------------------------------

    void drawFirstRunPanel() {
        createControllerIfReady();
        if (controller_) {
            return;
        }
        ImGui::SeparatorText("First run");
        if (bootstrapRunning_) {
            ImGui::TextUnformatted("Checking for FFmpeg and FFprobe...");
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f),
                               "downloading when needed");
        } else if (!bootstrapError_.empty()) {
            ImGui::TextColored(toneColor(StatusTone::Bad), "%s", bootstrapError_.c_str());
            if (ImGui::Button("Locate ffmpeg.exe...")) {
                const std::string path = genericFilePathDialog(L"Select ffmpeg.exe", false,
                                                               L"ffmpeg\0ffmpeg.exe\0All files\0*.*\0");
                if (!path.empty()) {
                    ffmpegPath_ = path;
                    const std::filesystem::path folder = std::filesystem::path(path).parent_path();
                    ffprobePath_ = (folder / "ffprobe.exe").string();
                    toolsReady_ = true;
                }
            }
        }
        ImGui::TextDisabled("MonoSect runs offline. The FFmpeg tools come from a pinned address and are identified by a SHA-256 before they run.");
        return;
    }

    void drawSourcePanel() {
        ImGui::SeparatorText("Source");
        if (!controller_) {
            ImGui::TextDisabled("Waiting for the toolchain.");
            return;
        }
        if (ImGui::Button("Add file...")) {
            addSourceFromDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove") && controller_->selected() != nullptr) {
            controller_->removeSource(controller_->selectedId());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(controller_->jobRunning() || controller_->selected() == nullptr);
        if (ImGui::Button("Read")) {
            controller_->probeSelected();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Probe the container and index every packet. Nothing is written.");
        }

        if (ImGui::BeginTable("##queue", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("##selected", ImGuiTableColumnFlags_WidthFixed, 18.0f);
            ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch);
            for (const QueueItem& item : controller_->queue()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool isSelected = item.id == controller_->selectedId();
                if (ImGui::RadioButton((std::string("##sel") + std::to_string(item.id)).c_str(), isSelected)) {
                    controller_->select(item.id);
                }
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Selectable(item.displayName.c_str(), isSelected)) {
                    controller_->select(item.id);
                }
                if (!item.problem.empty()) {
                    ImGui::TextColored(toneColor(StatusTone::Bad), "  %s", item.problem.c_str());
                } else if (item.probed) {
                    ImGui::TextDisabled("  %s", item.status.c_str());
                } else {
                    ImGui::TextDisabled("  %s", item.status.c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Drop files onto the window to queue them.");
    }

    void drawConstraintPanel() {
        ImGui::SeparatorText("Limits");
        if (!controller_) {
            ImGui::TextDisabled("Waiting for the toolchain.");
            return;
        }
        UserConstraint& constraint = controller_->constraint();

        // --- size limit ---------------------------------------------------------------
        if (ImGui::Checkbox("Maximum size per part", &sizeLimitEnabled_)) {
            applySettingsToController();
        }
        helpMarker(HelpContent::find(HelpTopicId::SizeLimit), [this](HelpTopicId id) { controller_->showHelp(id); });
        if (sizeLimitEnabled_) {
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputText("##sizevalue", sizeValueBuffer_, sizeof(sizeValueBuffer_),
                                 ImGuiInputTextFlags_CharsDecimal)) {
                applySettingsToController();
            }
            ImGui::SameLine();
            const char* units[] = {"GiB", "MiB", "KiB"};
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##sizeunit", &sizeUnitIndex_, units, 3)) {
                applySettingsToController();
            }
            const ByteCount bytes = parseSizeBytes(sizeValueBuffer_, sizeUnitIndex_);
            ImGui::TextDisabled("%s", formatBytesWithExact(bytes).c_str());
            ImGui::TextWrapped("%s", UiModel::limitExplanation(constraint).c_str());
        }

        // --- duration limit -----------------------------------------------------------
        if (ImGui::Checkbox("Maximum duration per part", &durationLimitEnabled_)) {
            applySettingsToController();
        }
        helpMarker(HelpContent::find(HelpTopicId::DurationLimit), [this](HelpTopicId id) { controller_->showHelp(id); });
        if (durationLimitEnabled_) {
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputText("##duration", durationValueBuffer_, sizeof(durationValueBuffer_), 0)) {
                applySettingsToController();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("hh:mm:ss or seconds");
        }

        ImGui::Spacing();
        const char* modes[] = {"Stream copy (bit-identical)", "Keyframe-aligned copy (default)",
                               "Smart cut (re-encode near the cuts)", "Full re-encode"};
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::Combo("How to cut##mode", &preservationIndex_, modes, 4)) {
            applySettingsToController();
        }
        helpMarker(HelpContent::find(HelpTopicId::PreservationModes), [this](HelpTopicId id) { controller_->showHelp(id); });
        ImGui::TextWrapped("%s", preservationModeGuarantee(constraint.preservation));
        ImGui::TextWrapped("%s", preservationModeDetail(constraint.preservation).c_str());

        const char* precisions[] = {"Fast", "Balanced", "Maximum (search for the closest fit)"};
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::Combo("How hard to try##precision", &precisionIndex_, precisions, 3)) {
            applySettingsToController();
        }
        helpMarker(HelpContent::find(HelpTopicId::Precision), [this](HelpTopicId id) { controller_->showHelp(id); });
        ImGui::TextWrapped("%s", precisionPolicyDetail(constraint.precision).c_str());

        if (ImGui::Checkbox("Limit the number of parts", &maxPartsEnabled_)) {
            applySettingsToController();
        }
        if (maxPartsEnabled_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputInt("##maxparts", &maxParts_, 1, 10)) {
                maxParts_ = std::max(1, maxParts_);
                applySettingsToController();
            }
            ImGui::TextDisabled("A plan that needs more parts is refused, never run with more.");
        }

        ImGui::SeparatorText("Output");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##pattern", namingBuffer_, sizeof(namingBuffer_))) {
            applySettingsToController();
        }
        helpMarker(HelpContent::find(HelpTopicId::OutputNaming), [this](HelpTopicId id) { controller_->showHelp(id); });
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##outdir", outputDirectoryBuffer_, sizeof(outputDirectoryBuffer_),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("...##outdir")) {
            const std::string folder = chooseFolderDialog(L"Output folder for the parts");
            if (!folder.empty()) {
                std::snprintf(outputDirectoryBuffer_, sizeof(outputDirectoryBuffer_), "%s", folder.c_str());
                controller_->setOutputDirectory(folder);
            }
        }

        // The constraint report is the engine's verdict, shown as is.
        const ConstraintReport report = controller_->validate();
        if (!report.valid) {
            for (const ConstraintIssue& issue : report.issues) {
                if (issue.severity == ConstraintIssue::Severity::Error) {
                    ImGui::TextColored(toneColor(StatusTone::Bad), "%s", issue.message.c_str());
                }
                if (!issue.remedy.empty()) {
                    ImGui::TextDisabled("  %s", issue.remedy.c_str());
                }
            }
        }
    }

    void drawRunPanel() {
        ImGui::SeparatorText("Run");
        if (!controller_) {
            ImGui::TextDisabled("Waiting for the toolchain.");
            return;
        }
        const bool running = controller_->jobRunning();
        const ConstraintReport report = controller_->validate();
        const bool canStart = report.valid && !running && controller_->selected() != nullptr;

        if (ImGui::Button("Preview plan", ImVec2(140.0f, 0.0f))) {
            controller_->previewPlan(controller_->outputDirectory());
        }
        helpMarker(HelpContent::find(HelpTopicId::Boundaries), [this](HelpTopicId id) { controller_->showHelp(id); });
        ImGui::SameLine();
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button("Start", ImVec2(140.0f, 0.0f))) {
            controller_->start(controller_->outputDirectory());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!running);
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            controller_->requestCancel();
        }
        ImGui::SameLine();
        if (controller_->snapshot().phase == SessionPhase::Paused) {
            if (ImGui::Button("Resume", ImVec2(100.0f, 0.0f))) {
                controller_->requestResume();
            }
        } else if (ImGui::Button("Pause after this part", ImVec2(180.0f, 0.0f))) {
            controller_->requestPause();
        }
        ImGui::EndDisabled();

        const ProgressView progress = controller_->progressView();
        const float fraction = static_cast<float>(progress.fraction);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), progress.describe().c_str());
        if (progress.etaExplanation.empty() && progress.secondsRemaining.has_value()) {
            ImGui::TextDisabled("The estimate comes from the parts already written; it is not a promise.");
        }
        const SessionSnapshot& snapshot = controller_->snapshot();
        if (!snapshot.stage.empty() && running) {
            ImGui::TextDisabled("%s", snapshot.stage.c_str());
        }
    }

    void drawPlanPanel() {
        ImGui::SeparatorText("Plan");
        if (!controller_) {
            return;
        }
        const std::optional<SplitPlan>& plan = controller_->preview();
        if (!plan.has_value()) {
            ImGui::TextDisabled("No plan yet. Set the limits and press Preview plan.");
            return;
        }
        ImGui::TextWrapped("%s", UiModel::planSummary(plan.value()).c_str());
        if (controller_->previewIsStale()) {
            ImGui::TextColored(toneColor(StatusTone::Warning),
                               "These limits changed after this plan was made. Plan again before starting.");
        }
        const std::vector<std::string> caveats = UiModel::planCaveats(plan.value());
        if (!caveats.empty()) {
            ImGui::TextColored(toneColor(StatusTone::Warning), "What this plan does not do:");
            for (const std::string& caveat : caveats) {
                ImGui::Bullet();
                ImGui::TextWrapped("%s", caveat.c_str());
            }
        }
        const std::vector<UiModel::PartLine> lines = controller_->partLines();
        if (lines.empty()) {
            return;
        }
        if (ImGui::BeginTable("##parts", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.0f);
            ImGui::TableSetupColumn("range");
            ImGui::TableSetupColumn("predicted size");
            ImGui::TableSetupColumn("method");
            ImGui::TableSetupColumn("boundary");
            ImGui::TableHeadersRow();
            for (const UiModel::PartLine& line : lines) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", line.index);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(line.range.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(line.size.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(line.method.c_str());
                ImGui::TableSetColumnIndex(4);
                if (line.exactBoundary) {
                    ImGui::TextColored(toneColor(StatusTone::Good), "%s", line.boundary.c_str());
                } else {
                    ImGui::TextUnformatted(line.boundary.c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Sizes are predictions until a part has been written and measured.");
    }

    void drawResultsPanel() {
        ImGui::SeparatorText("Results");
        if (!controller_) {
            return;
        }
        const JobStatusLine verification = controller_->verificationLine();
        ImGui::TextColored(toneColor(verification.tone), "%s", verification.headline.c_str());
        if (!verification.detail.empty()) {
            ImGui::TextWrapped("%s", verification.detail.c_str());
        }
        const SessionSnapshot& snapshot = controller_->snapshot();
        if (snapshot.parts.empty()) {
            ImGui::TextDisabled("Nothing has been written yet.");
            return;
        }
        if (ImGui::BeginTable("##results", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.0f);
            ImGui::TableSetupColumn("file");
            ImGui::TableSetupColumn("start - end", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("verified", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("note");
            ImGui::TableHeadersRow();
            for (const SegmentOutcome& part : snapshot.parts) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", part.index);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::filesystem::path(part.path).filename().string().c_str());
                ImGui::TableSetColumnIndex(2);
                const std::string range =
                    formatDuration(part.startSeconds) + " - " + formatDuration(part.endSeconds);
                ImGui::TextUnformatted(range.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(formatBytesShort(part.fileSizeBytes).c_str());
                ImGui::TableSetColumnIndex(4);
                if (part.verified) {
                    ImGui::TextColored(toneColor(StatusTone::Good), "verified");
                } else if (part.wrote) {
                    ImGui::TextColored(toneColor(StatusTone::Bad), "not verified");
                } else {
                    ImGui::TextDisabled("planned");
                }
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(part.verificationSummary.c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Open the output folder")) {
            const std::string folder = controller_->outputDirectory();
            if (!folder.empty()) {
                // The output folder is a UTF-8 std::string, and ShellExecuteW
                // needs UTF-16.  Widening it one byte at a time - which is what
                // std::wstring(begin, end) does - is not a conversion: each byte
                // becomes one wchar_t, so "é" (0xC3 0xA9) becomes two CJK-looking
                // characters.  Explorer then opens a folder that does not exist,
                // and the user is told nothing.  See core/Utf8Path.hpp.
                ::ShellExecuteW(nullptr, L"open", wideFromUtf8(folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }

    /// Runs HardwareCapabilityProbe on a worker thread and publishes the answer.
///
/// The probe is real work: `ffmpeg -encoders` plus one single-frame encode per
/// listed hardware encoder, each with its own deadline.  A machine with a slow or
/// wedged driver can take several seconds, and the window must stay responsive
/// throughout - a freeze on startup would be indistinguishable from a hang.
///
/// The result is immutable once published and written under a lock, so the UI thread
/// reads a consistent answer while the worker replaces it exactly once.
class BackgroundHardwareProbe final : public HardwareCapabilitySource {
public:
    BackgroundHardwareProbe(IProcessRunner& runner, std::string ffmpegPath)
        : runner_(runner), ffmpegPath_(std::move(ffmpegPath)) {}

    ~BackgroundHardwareProbe() override { join(); }

    void startProbe() override {
        std::lock_guard<std::mutex> guard(mutex_);
        if (worker_.joinable() || !workerFinished_.load()) {
            return;   // already running: a second probe would answer the same question
        }
        join();
        workerFinished_ = false;
        worker_ = std::thread([this] {
            HardwareCapabilityProbe probe(runner_, ffmpegPath_);
            HardwareCapabilities answer;
            try {
                answer = probe.probe();
            } catch (...) {
                // A probe that throws has told us it cannot answer.  Recording that
                // is the honest outcome; propagating it would take the window down
                // over a question about the graphics card.
                answer.probed = true;
                answer.probeError = "the hardware encoder probe did not finish";
            }
            {
                std::lock_guard<std::mutex> guard2(mutex_);
                result_ = std::move(answer);
            }
            workerFinished_ = true;
        });
    }

    bool probeRunning() const override { return !workerFinished_.load(); }

    HardwareCapabilities capabilities() const override {
        std::lock_guard<std::mutex> guard(mutex_);
        return result_;
    }

private:
    /// Joins a finished worker.  Called with no lock held, and never blocks on a
    /// probe that is still running.
    void join() {
        if (worker_.joinable() && workerFinished_.load()) {
            worker_.join();
        }
    }

    mutable std::mutex mutex_;
    IProcessRunner& runner_;
    std::string ffmpegPath_;
    HardwareCapabilities result_;
    std::thread worker_;
    std::atomic<bool> workerFinished_{true};
};

/// What this machine can actually encode on, and why.
    ///
    /// The distinction this panel exists to make is between an encoder FFmpeg
    /// *lists* and one that *works*.  A static FFmpeg build lists h264_nvenc,
    /// hevc_nvenc, h264_qsv and h264_amf whether or not the machine has the
    /// hardware, so a panel that reported the listing as availability would offer
    /// the user an encode that fails at run time - and, worse, would turn a
    /// requested stream copy into a re-encode without saying so.  So each listed
    /// encoder is shown with the result of actually asking it to encode a frame,
    /// and a failure names its own reason.
    void drawHardwarePanel() {
        if (!controller_) {
            return;
        }
        ImGui::TextUnformatted("Hardware encoders");

        if (controller_->hardwareProbeRunning()) {
            ImGui::TextDisabled("Testing each listed encoder with a real one-frame encode...");
            return;
        }
        if (!controller_->hardwareProbed()) {
            ImGui::TextDisabled("Not probed on this machine yet.");
            ImGui::TextDisabled("MonoSect does not assume a card is present, or that a listed encoder works.");
            return;
        }

        for (const EncoderProbe& encoder : controller_->hardwareEncoders()) {
            const char* icon = encoder.usable ? "[+]" : "[-]";
            ImGui::Text("%s %s  %s", icon, encoder.encoder.c_str(), encoder.vendor.c_str());
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                continue;
            }
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
            if (encoder.usable) {
                ImGui::TextUnformatted("A one-frame encode succeeded on this machine.");
            } else {
                ImGui::TextUnformatted(encoder.detail.empty() ? "Not usable on this machine." : encoder.detail.c_str());
            }
            if (!encoder.description.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("%s", encoder.description.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::TextWrapped("%s", controller_->hardwareSummary().c_str());
        helpMarker(HelpContent::find(HelpTopicId::GpuEncoders), [this](HelpTopicId id) { controller_->showHelp(id); });

        if (ImGui::Checkbox("Use a hardware encoder when a boundary must be re-encoded", &gpuEnabled_)) {
            controller_->setGpuEnabled(gpuEnabled_);
        }
        // The consequence is spelled out rather than left to be discovered: on a
        // copy-only split no encoder is needed, so this changes nothing, and saying
        // so is more useful than greying the box out and leaving the user to wonder.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextUnformatted(
                "A stream copy needs no encoder at all. This only applies when the boundary cannot be placed on a "
                "keyframe and has to be re-encoded.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (ImGui::Checkbox("Use hardware decoding", &hardwareDecodingEnabled_)) {
            controller_->setHardwareDecodingEnabled(hardwareDecodingEnabled_);
        }
    }

    void drawResourcesPanel() {
        ImGui::SeparatorText("Machine");
        if (!controller_) {
            return;
        }
        const ResourceSample& sample = controller_->sampleResources();
        auto meter = [&](const char* label, const MeasuredValue& value, const std::vector<float>& history,
                         const char* unit) {
            const std::string text = value.known ? UiModel::meterText(value, unit, 0) : std::string("n/a");
            ImGui::Text("%s: %s", label, text.c_str());
            if (!value.known && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", value.unavailableReason.c_str());
            }
            if (!history.empty()) {
                ImGui::PlotLines(std::string("##").append(label).c_str(), history.data(),
                                 static_cast<int>(history.size()), 0, nullptr, 0.0f, 100.0f, ImVec2(-1.0f, 42.0f));
            }
        };
        meter("CPU", sample.cpuPercent, cpuHistory_, "%");
        meter("Memory", sample.memoryPercent, memoryHistory_, "%");
        meter("GPU", sample.gpuPercent, gpuHistory_, "%");
        ImGui::Text("Disk read: %s", bytesPerSecondText(sample.diskReadBytesPerSecond).c_str());
        ImGui::Text("Disk write: %s", bytesPerSecondText(sample.diskWriteBytesPerSecond).c_str());
        ImGui::Text("Media written: %s", bytesPerSecondText(sample.mediaThroughputBytesPerSecond).c_str());
        ImGui::TextWrapped("%s", sample.sourceDescription.c_str());
        ImGui::Separator();

        drawHardwarePanel();

        ImGui::Separator();
        ResourcePolicyInput input = controller_->resourceInput();
        const ResourcePolicy policy = deriveResourcePolicy(input);
        ImGui::TextWrapped("%s", UiModel::resourcePolicySummary(policy).c_str());
        helpMarker(HelpContent::find(HelpTopicId::ResourceModes), [this](HelpTopicId id) { controller_->showHelp(id); });
        if (ImGui::Checkbox("Low priority", &lowPriority_)) {
            applySettingsToController();
        }
        if (ImGui::Checkbox("Limit encoder threads", &threadOverride_)) {
            applySettingsToController();
        }
        if (threadOverride_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputInt("##threads", &threads_, 1, 2)) {
                threads_ = std::max(1, threads_);
                applySettingsToController();
            }
        }
    }

    void drawHistoryPanel() {
        ImGui::SeparatorText("Job history");
        if (!controller_) {
            return;
        }
        if (ImGui::Button("Refresh")) {
            controller_->refreshHistory();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##historyfilter", "filter by name or state", historyFilter_,
                                 sizeof(historyFilter_));
        if (!controller_->historyProblem().empty()) {
            ImGui::TextColored(toneColor(StatusTone::Warning), "%s", controller_->historyProblem().c_str());
        }
        const std::string filter = historyFilter_;
        if (ImGui::BeginTable("##history", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("finished");
            ImGui::TableSetupColumn("source");
            ImGui::TableSetupColumn("parts");
            ImGui::TableSetupColumn("state");
            ImGui::TableHeadersRow();
            for (const JobRecord& record : controller_->history()) {
                if (!filter.empty()) {
                    const bool matches = record.sourcePath.find(filter) != std::string::npos ||
                                         record.finalState.find(filter) != std::string::npos;
                    if (!matches) {
                        continue;
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(record.finishedAt.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::filesystem::path(record.sourcePath).filename().string().c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", record.sourcePath.c_str());
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", record.parts.size());
                ImGui::TableSetColumnIndex(3);
                const bool good = record.finalState == "Completed";
                ImGui::TextColored(toneColor(good ? StatusTone::Good : StatusTone::Warning), "%s",
                                   record.finalState.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Stored at %s", databaseBuffer_);
    }

    void drawLogPanel() {
        ImGui::SeparatorText("Log");
        if (!controller_) {
            return;
        }
        if (ImGui::Button("Copy")) {
            std::string all;
            for (const std::string& line : controller_->snapshot().log) {
                all.append(line).append("\n");
            }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        ImGui::Checkbox("Follow", &logFollow_);
        ImGui::BeginChild("##loglines", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Border);
        for (const std::string& line : controller_->snapshot().log) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (logFollow_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    void drawHelpWindow() {
        if (!controller_ || !controller_->helpVisible()) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(860.0f, 620.0f), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("Help##helpwindow", &open)) {
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::InputTextWithHint("##helpsearch", "search the help", helpQueryBuffer_,
                                         sizeof(helpQueryBuffer_))) {
                controller_->setHelpQuery(helpQueryBuffer_);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu topic(s)", controller_->helpResults().empty()
                                                    ? HelpContent::topics().size()
                                                    : controller_->helpResults().size());
            ImGui::Separator();
            ImGui::BeginChild("##topics", ImVec2(260.0f, 0.0f), ImGuiChildFlags_Border);
            const std::vector<const HelpTopic*>& results =
                helpQueryBuffer_[0] == '\0' ? allTopics() : controller_->helpResults();
            for (const HelpTopic* topic : results) {
                if (topic == nullptr) {
                    continue;
                }
                const bool selected = controller_->helpTopic() == topic;
                if (ImGui::Selectable(topic->title.c_str(), selected)) {
                    controller_->showHelp(topic->id);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", topic->oneLine.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##topicbody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Border);
            const HelpTopic* topic = controller_->helpTopic();
            if (topic != nullptr) {
                ImGui::TextColored(toneColor(StatusTone::Working), "%s", topic->title.c_str());
                ImGui::TextWrapped("%s", topic->oneLine.c_str());
                ImGui::Separator();
                ImGui::TextWrapped("%s", topic->body.c_str());
                ImGui::Spacing();
                ImGui::SeparatorText("Example");
                ImGui::TextWrapped("%s", topic->example.c_str());
                if (!topic->seeAlso.empty()) {
                    ImGui::SeparatorText("See also");
                    for (const std::string& id : topic->seeAlso) {
                        const HelpTopic* other = HelpContent::findByText(id);
                        if (other == nullptr) {
                            continue;
                        }
                        if (ImGui::Button(other->title.c_str())) {
                            controller_->showHelp(other->id);
                        }
                        ImGui::SameLine();
                    }
                    ImGui::NewLine();
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
        if (!open) {
            controller_->closeHelp();
        }
    }

    static const std::vector<const HelpTopic*>& allTopics() {
        static std::vector<const HelpTopic*> topics = [] {
            std::vector<const HelpTopic*> pointers;
            for (const HelpTopic& topic : HelpContent::topics()) {
                pointers.push_back(&topic);
            }
            return pointers;
        }();
        return topics;
    }

    void drawAboutWindow() {
        if (!showAbout_) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 320.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("About MonoSect", &showAbout_)) {
            ImGui::Text("MonoSect %s", version_.c_str());
            ImGui::TextDisabled("Precision media segmentation for Windows 10 and 11.");
            ImGui::Separator();
            ImGui::TextWrapped(
                "MonoSect splits a media file into parts that never exceed the limits you set. The limits are hard: "
                "a part is never published above the size in bytes or the duration you asked for, and every written "
                "part is verified before it is reported as finished.");
            ImGui::Separator();
            ImGui::Text("FFmpeg: %s", ffmpegPath_.empty() ? "not ready" : ffmpegPath_.c_str());
            ImGui::Text("FFprobe: %s", ffprobePath_.empty() ? "not ready" : ffprobePath_.c_str());
            ImGui::TextDisabled("The tools are identified before they run: a pinned release SHA-256 when this build "
                                "defines one, and the recorded hash of the download otherwise. Their versions are "
                                "recorded in every job record.");
        }
        ImGui::End();
    }

    void sampleResources() {
        const double now = ImGui::GetTime();
        if (now - lastResourceSample_ < 1.0) {
            return;
        }
        lastResourceSample_ = now;
        const ResourceSample& sample = controller_->sampleResources();
        pushHistory(cpuHistory_, sample.cpuPercent);
        pushHistory(memoryHistory_, sample.memoryPercent);
        pushHistory(gpuHistory_, sample.gpuPercent);
    }

    static void pushHistory(std::vector<float>& history, const MeasuredValue& value) {
        if (!value.known) {
            // An unavailable metric leaves a gap in the graph rather than a zero
            // that would read as "idle" (TODO 206/207).
            return;
        }
        history.push_back(static_cast<float>(value.value));
        if (history.size() > 180) {
            history.erase(history.begin(), history.begin() + 60);
        }
    }

    // ---- state ----------------------------------------------------------------------

    GLFWwindow* window_ = nullptr;
    bool imguiReady_ = false;
    std::string version_;
    std::filesystem::path executableDirectory_;

    std::unique_ptr<FFprobeService> prober_;
    std::unique_ptr<EngineUiEngine> engine_;
    std::unique_ptr<UiController> controller_;
    std::unique_ptr<BackgroundHardwareProbe> hardware_;

    std::jthread bootstrapThread_;
    std::atomic<bool> toolsReady_{false};
    bool bootstrapRunning_ = false;
    std::string bootstrapError_;
    std::vector<std::string> bootstrapLog_;
    std::mutex bootstrapMutex_;
    std::string ffmpegPath_;
    std::string ffprobePath_;
    std::vector<std::string> logLines_;

    char outputDirectoryBuffer_[512] = {};
    char namingBuffer_[256] = {};
    char sizeValueBuffer_[64] = {};
    char durationValueBuffer_[64] = {};
    char databaseBuffer_[512] = {};
    char historyFilter_[64] = {};
    char helpQueryBuffer_[128] = {};
    int sizeUnitIndex_ = 1;
    bool sizeLimitEnabled_ = true;
    bool durationLimitEnabled_ = false;
    int preservationIndex_ = 1;
    int precisionIndex_ = 1;
    bool maxPartsEnabled_ = false;
    int maxParts_ = 10;
    bool verifyStreams_ = true;
    bool verifyKeyframeStart_ = true;
    bool verifyDecode_ = false;
    bool lowPriority_ = false;
    bool threadOverride_ = false;
    bool gpuEnabled_ = true;
    bool hardwareDecodingEnabled_ = true;
    int threads_ = 4;
    bool showResources_ = true;
    bool showHistory_ = true;
    bool showLog_ = true;
    bool showAbout_ = false;
    bool logFollow_ = true;
    double lastResourceSample_ = 0.0;
    std::vector<float> cpuHistory_;
    std::vector<float> memoryHistory_;
    std::vector<float> gpuHistory_;
};

int runApplicationWindow(std::string appVersion) {
    MainWindow window(appVersion.empty() ? std::string(MONOSECT_VERSION_STR) : std::move(appVersion));
    if (!window.initialize()) {
        MessageBoxW(nullptr, L"MonoSect could not open its window.", L"MonoSect", MB_ICONERROR | MB_OK);
        return 1;
    }
    return window.run();
}

} // namespace monosect::ui
