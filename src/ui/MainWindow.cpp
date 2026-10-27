// MonoSect — the application window (Windows only; see MainWindow.hpp).
#include "ui/MainWindow.hpp"

#include "core/Bytes.hpp"
#include "core/DependencyPin.hpp"
#include "core/Utf8Path.hpp"
#include "engine/FFmpegBootstrap.hpp"
#include "engine/ProcessRunners.hpp"
#include "engine/WatchFolder.hpp"
#include "ui/Branding.hpp"
#include "ui/UiController.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>

// glfwGetWin32Window is the only way to get the real HWND, which the shell folder
// dialogs need as their owner: GLFW focuses with SetForegroundWindow, so
// GetActiveWindow() returns NULL and the dialog has no owner.  Declared after
// windows.h deliberately - glfw3native.h would include it itself, and having the GL
// headers see windows.h first is the arrangement that avoids the classic
// wingdi/GL header clash.
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

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

std::string chooseFolderDialog(const wchar_t* title, HWND owner) {
    // SHBrowseForFolderW is a COM call and returns S_OK-less failure on a thread that
    // never called CoInitializeEx - which is not an error the caller can distinguish
    // from the user cancelling.  The folder pickers therefore did nothing at all, and
    // silently, on a process that initialised COM nowhere.  COM is initialised once on
    // the window thread in MainWindow::initialize() and released in its destructor;
    // this only re-checks, because a dialog invoked before initialization would
    // otherwise fail in exactly the same invisible way.
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool mustUninitialize = SUCCEEDED(com);

    BROWSEINFOW info{};
    // GetActiveWindow() is nearly always NULL for a GLFW window - GLFW focuses with
    // SetForegroundWindow, which does not make a window "active" in that sense - so
    // the dialog had no owner and could open behind the application.  The real HWND
    // is passed in instead.
    info.hwndOwner = owner != nullptr ? owner : GetActiveWindow();
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE items = SHBrowseForFolderW(&info);

    std::string result;
    if (items != nullptr) {
        // MAX_PATH is what SHGetPathFromIDListW documents; the deep-path limit is
        // handled by asking the shell for the path instead.
        std::vector<wchar_t> path(32768, L'\0');
        if (SHGetPathFromIDListW(items, path.data()) == TRUE) {
            result = utf8FromWide(path.data());
        }
        CoTaskMemFree(items);
    }
    if (mustUninitialize) {
        CoUninitialize();
    }
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
        // Every worker this object owns has to be stopped and joined *before* the
        // members that hold them are destroyed.  A std::thread that is still joinable
        // when its object is destroyed calls std::terminate, so a window closed during
        // a merge - or while the hardware probe is still running - used to take the
        // process down on the way out rather than closing.
        if (bootstrapThread_.joinable()) {
            bootstrapThread_.request_stop();
            bootstrapThread_.join();
        }
        if (controller_) {
            controller_->requestMergeCancel();
            controller_->waitForMerge();
            controller_->requestCancel();
            controller_->waitForJob();
        }
        // Stopping the watch is what stops the watcher's thread from pushing into
        // watchedFiles_, which is a member that would already be destroyed by then.
        if (watchSource_) {
            watchSource_->stopWatch();
        }
        // The probe outlives this body unless it is reaped here: its worker captures
        // `this` and touches the mutex and result, and its thread object is a member.
        hardware_.reset();
        controller_.reset();
        engine_.reset();
        prober_.reset();
        if (logoTexture_.valid()) {
            // Freed while the GL context is still alive, which is the only time a
            // texture can actually be deleted.
            vsm::releaseTexture(logoTexture_);
        }
        if (imguiReady_) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
        }
        glfwTerminate();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        g_window = nullptr;
    }

    /// Makes the process per-monitor DPI aware, before any window exists.
    ///
    /// DPI awareness is a process-wide property that can only be set before the first
    /// window is created, and getting it wrong is invisible in code and obvious on
    /// screen.  Without it the window is handed to MonoSect in virtualised 96 dpi units
    /// while the pixels it actually receives are physical: on a 125 % display the
    /// window MonoSect asked for as 1500x900 was really 1517x947, the framebuffer and
    /// the window disagreed, and the columns were laid out across a surface wider than
    /// the window.  The right-hand column - the machine readings and the job history -
    /// was drawn past the right edge and could not be seen or reached at all.
    ///
    /// The newest API is tried first and the older ones are kept as fallbacks, so this
    /// still works on a pre-1703 Windows 10.  Each is resolved dynamically rather than
    /// linked, because SetProcessDpiAwarenessContext does not exist before Windows 10
    /// 1703 and a static import would refuse to load there.
    using SetContext = BOOL(WINAPI*)(HANDLE);
    using SetAware = BOOL(WINAPI*)();

    static void makeDpiAware() {
        // GetProcAddress returns FARPROC, which is not the same type as the function
        // pointer being cast to; casting between them directly is a warning under
        // -Wcast-function-type and is exactly the kind of cast that is not guaranteed
        // to work.  Copying the representation is the portable way to do it, and the
        // sizes are asserted rather than assumed.
        const auto resolve = [](HMODULE module, const char* name) -> void* {
            if (module == nullptr) {
                return nullptr;
            }
            FARPROC address = GetProcAddress(module, name);
            void* callable = nullptr;
            static_assert(sizeof(callable) == sizeof(address), "unexpected FARPROC size");
            std::memcpy(&callable, &address, sizeof(callable));
            return callable;
        };

        // Windows 10 1703+: per-monitor v2, which rescales when the window moves
        // between monitors of different scale.
        if (const auto perMonitorV2 = reinterpret_cast<SetContext>(
                resolve(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle -4.
            if (perMonitorV2(reinterpret_cast<HANDLE>(-4))) {
                return;
            }
        }
        // Windows 8.1: system aware, which is enough to stop the virtualisation that
        // puts the columns outside the window.
        if (const auto systemAware =
                reinterpret_cast<SetAware>(resolve(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware"))) {
            if (systemAware()) {
                return;
            }
        }
    }

    /// The native HWND behind the GLFW window, for the shell dialogs.
    ///
    /// GLFW focuses with SetForegroundWindow, so GetActiveWindow() returns NULL here
    /// and an ownerless dialog can open behind the application.  Null before the
    /// window exists, which the folder dialog handles.
    HWND nativeWindow() const {
        return window_ != nullptr ? glfwGetWin32Window(window_) : nullptr;
    }

    bool initialize(bool smokeTest = false) {
        // Before glfwInit: DPI awareness is set once per process and must be in place
        // before a window exists, so it cannot be left to whoever creates one.
        makeDpiAware();
        if (glfwInit() == 0) {
            return false;
        }
        // The shell folder and file dialogs are COM clients, and a thread that has not
        // initialised COM makes them fail in a way indistinguishable from the user
        // cancelling.  Done here, on the one thread that owns the window, and undone in
        // the destructor.  S_FALSE means COM was already up on this thread, which is
        // also success and must not be undone here.
        comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
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
        if (smokeTest) {
            io.IniFilename = nullptr;
        }
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        applyTheme();
        float xscale = 1.0f;
        float yscale = 1.0f;
        glfwGetWindowContentScale(window_, &xscale, &yscale);
        // The glyph ranges are the point of this call being written out rather than
        // left at AddFontFromFileTTF's default.
        //
        // The default is GetGlyphRangesDefault(): Basic Latin plus Latin Supplement.  This
        // product's whole reason for a core/Utf8Path.hpp is that it handles filenames in
        // any language - and the end-to-end suite proves it on filenames like
        // "video été ???.mp4" - yet the font had no glyphs for a Cyrillic or Greek
        // letter, so such a filename was displayed as "???" in the results table and the
        // job history.  The tool was correct and the display was lying about it.
        //
        // Latin, Latin-1 Supplement, Latin Extended-A and Cyrillic, built explicitly.
        // CJK is not included: it would multiply the atlas size by an order of magnitude
        // for filenames that are rare here, and a missing glyph is a visible "?" rather
        // than a crash or a wrong path.
        static ImWchar glyphRanges[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
            0x0100, 0x017F,   // Latin Extended-A
            0x0400, 0x04FF,   // Cyrillic
            0x2010, 0x2027,   // dashes and quotes used in filenames
            0,
        };
        // Taken from the Windows directory rather than written as a literal absolute
        // path: "C:\Windows\Fonts" is not the font directory on every installation,
        // and on a relocated Windows the literal would silently fall through to the
        // default font.
        std::wstring windowsDir(32768, L'\0');
        const DWORD windowsLength = GetWindowsDirectoryW(windowsDir.data(),
                                                        static_cast<DWORD>(windowsDir.size()));
        std::filesystem::path fontPath = L"segoeui.ttf";
        if (windowsLength > 0 && windowsLength < windowsDir.size()) {
            fontPath = std::filesystem::path(std::wstring(windowsDir.data(), windowsLength)) / L"Fonts" /
                       L"segoeui.ttf";
        }
        if (io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), std::round(16.0f * xscale), nullptr,
                                        glyphRanges) == nullptr) {
            io.Fonts->AddFontDefault();
        }
        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init("#version 330");
        imguiReady_ = true;
        loadLogo();

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
        // utf8FromPath, not path::string(): a wide path converted with .string() comes
        // back in the ANSI code page, so a MonoSect installed under a folder with a
        // non-ASCII character was handed its own path back unreadable - as the default
        // output folder and as the job-history database path.
        std::snprintf(outputDirectoryBuffer_, sizeof(outputDirectoryBuffer_), "%s",
                      utf8FromPath(executableDirectory_ / "output").c_str());
        std::snprintf(namingBuffer_, sizeof(namingBuffer_), "%s", "{name} - part {index:03}");
        std::snprintf(sizeValueBuffer_, sizeof(sizeValueBuffer_), "%s", "700");
        std::snprintf(durationValueBuffer_, sizeof(durationValueBuffer_), "%s", "00:10:00");
        std::snprintf(databaseBuffer_, sizeof(databaseBuffer_), "%s",
                      utf8FromPath(JobStore::defaultStore().path()).c_str());
        if (smokeTest) {
            // Check the pins compiled into this executable, not only the packaging script's JSON.
            if (!verifyToolFile(executableDirectory_ / "ffmpeg.exe", pinnedFfmpeg(), false).ok() ||
                !verifyToolFile(executableDirectory_ / "ffprobe.exe", pinnedFfprobe(), false).ok()) {
                return false;
            }
        } else {
            startToolBootstrap();
        }
        return true;
    }

    int run(bool smokeTest = false) {
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
            if (smokeTest) {
                return 0;
            }
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
        bootstrapThread_ = std::jthread([this](std::stop_token stop) {
            const bool ok = vsm::ensureFFmpeg(executableDirectory_, [this](const std::string& line) {
                std::lock_guard<std::mutex> lock(bootstrapMutex_);
                bootstrapLog_.push_back(line);
            }, stop);
            if (!ok) {
                std::lock_guard<std::mutex> lock(bootstrapMutex_);
                bootstrapLog_.push_back(kBootstrapFailureMark +
                                        std::string("FFmpeg and FFprobe could not be prepared. Place ffmpeg.exe "
                                                    "and ffprobe.exe next to MonoSect.exe, or point MonoSect at "
                                                    "an existing installation."));
                return;
            }
            // The lock is held across the assignment and the log line, not just around
            // the log line.  These two strings are also written by the "Locate
            // ffmpeg.exe..." button on the UI thread, and a std::string written from two
            // threads at once can be left with a dangling pointer inside itself - a
            // crash far away from here, or a filename built from freed memory.
            {
                std::lock_guard<std::mutex> lock(bootstrapMutex_);
                // Only if the user has not already pointed MonoSect somewhere else.  A
                // download that finishes after an explicit choice must not silently
                // replace the choice the user made.
                if (ffmpegPath_.empty()) {
                    ffmpegPath_ = utf8FromPath(executableDirectory_ / "ffmpeg.exe");
                }
                if (ffprobePath_.empty()) {
                    ffprobePath_ = utf8FromPath(executableDirectory_ / "ffprobe.exe");
                }
                bootstrapLog_.push_back("FFmpeg ready: " + ffmpegPath_);
            }
            toolsReady_ = true;
            bootstrapRunning_ = false;
        });
    }

    /// The controller needs the tools and is therefore created on the UI thread
    /// once they exist.
    void createControllerIfReady() {
        if (controller_ || !toolsReady_) {
            return;
        }
        // Copied under the lock rather than read straight: toolsReady_ being true means
        // the writer published the paths before the flag, but reading them without the
        // lock still races the write of the std::string itself.
        std::string ffmpeg;
        std::string ffprobe;
        {
            std::lock_guard<std::mutex> lock(bootstrapMutex_);
            ffmpeg = ffmpegPath_;
            ffprobe = ffprobePath_;
        }
        if (ffmpeg.empty() || ffprobe.empty()) {
            return;   // flagged ready with nothing to run; nothing sensible to build
        }
        prober_ = std::make_unique<FFprobeService>(windowsProcessRunner(), ffprobe);
        engine_ = std::make_unique<EngineUiEngine>(*prober_, windowsProcessRunner(), ffmpeg);
        hardware_ = std::make_unique<BackgroundHardwareProbe>(windowsProcessRunner(), ffmpeg);
        watcher_ = std::make_unique<WatchFolder>();
        watchSource_ = std::make_unique<EngineWatchFolder>(*watcher_, watchQueueMutex_, watchedFiles_);
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
    }

    static ByteCount parseSizeBytes(const char* text, int unitIndex) {
        const char* unit = unitIndex == 0 ? " GiB" : unitIndex == 1 ? " MiB" : " KiB";
        const auto parsed = parseByteSize(std::string(text) + unit);
        return parsed.has_value() ? parsed.value().bytes : 0;
    }

    static Rational parseDurationSeconds(const char* text) {
        return parseDuration(text).value_or(Rational{0, 1});
    }

    void addSourceFromDialog() {
        if (!controller_ || controller_->jobRunning() || controller_->mergeRunning()) {
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
            if (controller_->mergeVisible()) {
                controller_->previewMerge(mergeOutputBuffer_, mergeNameBuffer_);
            } else {
                controller_->previewPlan(controller_->outputDirectory());
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            if (controller_->mergeVisible()) { controller_->startMerge(); }
            else { controller_->start(controller_->outputDirectory()); }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            controller_->requestCancel();
            controller_->requestMergeCancel();
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
                    const std::string folder = chooseFolderDialog(L"Output folder for the parts", nativeWindow());
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
                const bool canSplit = controller_ && !running && !controller_->mergeRunning() && !controller_->mergeVisible();
                if (ImGui::MenuItem("Read the selected source", nullptr, false, canSplit)) {
                    controller_->probeSelected();
                }
                if (ImGui::MenuItem("Preview plan", "Ctrl+Enter", false, canSplit)) {
                    controller_->previewPlan(controller_->outputDirectory());
                }
                if (ImGui::MenuItem("Start", "F5", false, canSplit)) {
                    controller_->start(controller_->outputDirectory());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Pause at the next part boundary", nullptr, false, running)) {
                    controller_->requestPause();
                }
                if (ImGui::MenuItem("Resume", nullptr, false, running)) {
                    controller_->requestResume();
                }
                if (ImGui::MenuItem("Cancel", "Esc", false, running || (controller_ && controller_->mergeRunning()))) {
                    controller_->requestCancel();
                    controller_->requestMergeCancel();
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

    /// Loads the branding, once, after the GL context exists.
    ///
    /// The files are copied next to the executable by the build, so the paths are
    /// derived from where the program is rather than from the working directory -
    /// which is not the executable's directory when the user starts it from a
    /// shortcut or a file manager, and a logo that only appears when launched from a
    /// terminal is worse than none.
    ///
    /// A missing or unreadable file is not an error: the window draws without it and
    /// the window icon falls back to whatever the system provides.  Branding should
    /// never be the reason the product does not start.
    void loadLogo() {
        const std::filesystem::path here = vsm::executableDirectory();

        // The title bar and the taskbar.  GLFW's own path to these is
        // LoadImageW(hInstance, L"GLFW_ICON") (win32_window.c), which now resolves
        // because resources/Monosect.rc declares the icon under that name as well as
        // under a numeric id.  Setting it explicitly as well is deliberate: it does
        // not depend on the resource script staying in step with GLFW's internals,
        // and glfwSetWindowIcon is the documented way to change a window's icon.
        const std::filesystem::path square = here / "Monosect-icon-square.png";
        if (std::filesystem::exists(square)) {
            vsm::ImageRgba squareImage;
            if (vsm::loadPngAsRgba(square, squareImage) && window_ != nullptr) {
                GLFWimage icon{};
                icon.width = squareImage.width;
                icon.height = squareImage.height;
                icon.pixels = squareImage.pixels.data();
                glfwSetWindowIcon(window_, 1, &icon);
            }
        }

        // The mark in the left column.  Reversed to a light mark on transparency, so
        // it sits on the dark theme without a white plate behind it.
        const std::filesystem::path logo = here / "Monosect-full-logo.png";
        if (!std::filesystem::exists(logo)) {
            return;
        }
        if (!vsm::loadMarkAsTexture(logo, logoTexture_, 0xEC, 0xEC, 0xEF)) {
            logoTexture_ = vsm::TextureHandle{};
        }
    }

    void drawLogo() {
        if (!logoTexture_.valid()) {
            return;
        }
        const float available = ImGui::GetContentRegionAvail().x;
        if (available <= 1.0f) {
            // A column the user has dragged to almost nothing.  Drawing here would
            // compute a size from a near-zero width and produce a degenerate quad.
            return;
        }
        // Fitted to the column rather than to a fixed 380 px, because the column is
        // now resizable: a fixed size would overflow a narrow column and waste space
        // in a wide one.
        float width = available;
        float height =
            width * static_cast<float>(logoTexture_.height) / static_cast<float>(logoTexture_.width);
        // Never taller than a quarter of the window: a very wide logo file would
        // otherwise push every panel below it off the screen.
        const float maxHeight = ImGui::GetIO().DisplaySize.y * 0.25f;
        if (height > maxHeight) {
            height = maxHeight;
            width = height * static_cast<float>(logoTexture_.width) /
                    static_cast<float>(logoTexture_.height);
        }
        // Re-clamped to the column.  A wide logo file on a short window makes the
        // height-first branch produce a width *wider* than the column, which is the
        // overflow the fit-to-width branch above exists to prevent - it just needed a
        // second trigger.
        if (width > available) {
            width = available;
            height = width * static_cast<float>(logoTexture_.height) /
                     static_cast<float>(logoTexture_.width);
        }
        ImGui::SetCursorPosX((available - width) * 0.5f);
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(logoTexture_.id)), ImVec2(width, height));
        ImGui::Spacing();
    }

    /// A drag handle between two column boundaries.
    ///
    /// `width` is the column this handle's left edge controls; it is modified in place
    /// while the handle is dragged and clamped by the caller afterwards.
    ///
    /// Two decisions worth stating.  First, the handle is *drawn*: there is a dimmed
    /// line in the gutter that brightens under the cursor and turns the cursor into a
    /// resize cursor.  A divider you cannot see is a divider you never find, and there
    /// was nothing at all on screen to say these columns could move.
    ///
    /// Second, the drag is tracked from the global mouse state rather than from the
    /// item's own active/id-considered state.  Grabbing on press anywhere in the
    /// gutter and then following the cursor until release is both what a divider is
    /// expected to do - the drag does not stop the instant the cursor leaves a 7 px
    /// strip - and independent of the per-item activation rules, which is what the
    /// previous item-active version depended on and which did not fire here.
    void dragColumnSplitter(const char* id, float& width) {
        ImGui::SameLine(0.0f, 0.0f);
        const float height = ImGui::GetContentRegionAvail().y;
        ImGui::InvisibleButton(id, ImVec2(kSplitterWidth, height), ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        // The grip, drawn last so it is not covered by anything the sibling columns
        // emit afterwards.
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const bool active = splitterDragging_ || hovered;
        const ImU32 colour = ImGui::GetColorU32(active ? ImVec4(0.55f, 0.62f, 0.72f, 1.0f)
                                                        : ImVec4(0.24f, 0.26f, 0.30f, 1.0f));
        const float centre = (min.x + max.x) * 0.5f;
        const float thickness = active ? 3.0f : 1.0f;
        const float top = min.y + ImGui::GetTextLineHeight();
        const float bottom = max.y - ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(centre - thickness * 0.5f, top),
                                                  ImVec2(centre + thickness * 0.5f, bottom), colour);

        if (!splitterDragging_) {
            // IsMouseClicked is the press transition in this ImGui version - "went from
            // !Down to Down" - which is what starts a press-and-hold drag.  (There is
            // no IsMousePressed in this version; on older ones IsMouseClicked means a
            // completed click and the start of a drag needs the transition test.)
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                splitterDragging_ = true;
                splitterStartX_ = ImGui::GetIO().MousePos.x;
                splitterStartWidth_ = width;
            }
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            splitterDragging_ = false;
            return;
        }
        width = splitterStartWidth_ + (ImGui::GetIO().MousePos.x - splitterStartX_);
    }

    void drawDockspace() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float statusBarHeight = ImGui::GetFrameHeightWithSpacing() + 8.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
        ImGui::Begin("##workspace", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollbar);

        // Split and merge are separate panels rather than one panel with a mode
        // switch.  They share almost nothing: a split is bounded by a size or
        // duration limit and verified part by part, a merge has no limit at all and
        // is verified as one output.  Merging them behind a toggle would mean
        // showing a constraint editor that cannot apply to what the user picked.
        const bool merging = controller_ != nullptr && controller_->mergeVisible();
        const bool rightColumn = showResources_ || showHistory_;

        // Widths are resolved before anything is drawn, from the width actually
        // available this frame, so a window that has been shrunk cannot produce a
        // negative middle column.  ImGui does not treat a negative size as an error:
        // CalcItemSize turns it into ImMax(4, avail + size), which silently became a
        // 4-pixel sliver for the middle panel while the right panel collapsed to its
        // minimum - a layout that was already reachable at 420 + 340 + 8 = 768 px and
        // became trivial to reach once the columns were draggable.
        const float total = ImGui::GetContentRegionAvail().x;
        const float handles = rightColumn ? kSplitterWidth * 2.0f : kSplitterWidth;
        const float roomForColumns = std::max(0.0f, total - handles);

        leftWidth_ = std::clamp(leftWidth_, kMinLeftWidth, std::max(kMinLeftWidth, roomForColumns * 0.6f));
        rightWidth_ = std::clamp(rightWidth_, kMinRightWidth,
                                 std::max(kMinRightWidth, roomForColumns * 0.6f));
        // The middle column is where the work happens, so it is the last thing
        // squeezed: the side columns give up width before it does.
        if (roomForColumns - leftWidth_ - rightWidth_ < kMinMiddleWidth) {
            const float wanted = roomForColumns - leftWidth_ - kMinMiddleWidth;
            rightWidth_ = std::min(rightWidth_, std::max(0.0f, wanted));
        }

        const float middleWidth =
            std::max(0.0f, roomForColumns - leftWidth_ - (rightColumn ? rightWidth_ : 0.0f));

        ImGui::BeginChild("##left", ImVec2(leftWidth_, 0.0f), ImGuiChildFlags_Border);
        if (!merging) {
            drawLogo();
            drawFirstRunPanel();
            drawSourcePanel();
            drawConstraintPanel();
        }
        ImGui::EndChild();

        // The left column was drawn at the width this frame resolved, which is the
        // width before this frame's drag - the drag therefore takes effect on the next
        // frame.  A one-frame lag on a 60 Hz redraw is not perceptible, and resolving
        // widths before drawing is what keeps a negative middle column impossible.
        dragColumnSplitter("##splitterLeft", leftWidth_);

        // SameLine(0, 0) and not SameLine(): the default carries ItemSpacing.x (8 px),
        // which on top of the 7 px handle made the row 8 px wider than the width the
        // arithmetic above had budgeted - so the right-hand column was pushed off the
        // edge of the window and simply was not there.  The handle is the gap.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##middle", ImVec2(middleWidth, 0.0f), ImGuiChildFlags_Border);
        if (merging) {
            drawMergePanel();
        } else {
            drawRunPanel();
            drawPlanPanel();
            drawResultsPanel();
            if (showLog_) {
                drawLogPanel();
            }
        }
        ImGui::EndChild();

        if (rightColumn) {
            // The right column is pinned to the window edge, so the handle that
            // controls its left edge grows the column as it moves left.
            dragColumnSplitter("##splitterRight", rightWidth_);

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::BeginChild("##right", ImVec2(rightWidth_, 0.0f), ImGuiChildFlags_Border);
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
            drainWatchedFiles();
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
                    // Under the lock, because the bootstrap thread writes these same
                    // two strings; see startToolBootstrap().
                    std::lock_guard<std::mutex> lock(bootstrapMutex_);
                    const std::filesystem::path folder = std::filesystem::path(wideFromUtf8(path)).parent_path();
                    const auto ffmpegCheck = verifyToolFile(std::filesystem::path(wideFromUtf8(path)), pinnedFfmpeg());
                    const auto ffprobeCheck = verifyToolFile(folder / "ffprobe.exe", pinnedFfprobe());
                    if (!ffmpegCheck.ok() || !ffprobeCheck.ok()) {
                        bootstrapError_ = !ffmpegCheck.ok() ? ffmpegCheck.message : ffprobeCheck.message;
                        return;
                    }
                    ffmpegPath_ = path;
                    ffprobePath_ = utf8FromPath(folder / "ffprobe.exe");
                    toolsReady_ = true;
                    // The explicit choice ends the "still working" state, which
                    // otherwise only the failure path ever cleared - leaving a progress
                    // bar animating forever next to a working toolchain.
                    bootstrapRunning_ = false;
                    bootstrapError_.clear();
                }
            }
        }
        ImGui::TextWrapped("Media stays on your computer. The first run downloads FFmpeg if needed; its SHA-256 is checked before use.");
        return;
    }

    void drawSourcePanel() {
        ImGui::SeparatorText("Source");
        if (!controller_) {
            ImGui::TextDisabled("Waiting for the toolchain.");
            return;
        }
        ImGui::BeginDisabled(controller_->jobRunning() || controller_->mergeRunning());
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
        ImGui::EndDisabled();
    }

    void drawConstraintPanel() {
        ImGui::SeparatorText("Limits");
        if (!controller_) {
            ImGui::TextDisabled("Waiting for the toolchain.");
            return;
        }
        UserConstraint& constraint = controller_->constraint();
        ImGui::BeginDisabled(controller_->jobRunning() || controller_->mergeRunning());

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
        ImGui::TextUnformatted("How to cut");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##mode", &preservationIndex_, modes, 4)) {
            applySettingsToController();
        }
        helpMarker(HelpContent::find(HelpTopicId::PreservationModes), [this](HelpTopicId id) { controller_->showHelp(id); });
        ImGui::TextWrapped("%s", preservationModeGuarantee(constraint.preservation));
        ImGui::TextWrapped("%s", preservationModeDetail(constraint.preservation).c_str());

        const char* precisions[] = {"Fast", "Balanced", "Maximum (search for the closest fit)"};
        ImGui::TextUnformatted("How hard to try");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##precision", &precisionIndex_, precisions, 3)) {
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
        ImGui::TextUnformatted("File name pattern");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##pattern", namingBuffer_, sizeof(namingBuffer_))) {
            applySettingsToController();
        }
        helpMarker(HelpContent::find(HelpTopicId::OutputNaming), [this](HelpTopicId id) { controller_->showHelp(id); });
        ImGui::TextUnformatted("Destination folder");
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##outdir", outputDirectoryBuffer_, sizeof(outputDirectoryBuffer_),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("...##outdir", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            const std::string folder = chooseFolderDialog(L"Output folder for the parts", nativeWindow());
            if (!folder.empty()) {
                std::snprintf(outputDirectoryBuffer_, sizeof(outputDirectoryBuffer_), "%s", folder.c_str());
                controller_->setOutputDirectory(folder);
            }
        }
        ImGui::SeparatorText("Verification");
        ImGui::TextWrapped("Size, duration, streams and the first keyframe are checked before each part is published.");
        if (ImGui::Checkbox("Full decode check (slower)", &verifyDecode_)) {
            applySettingsToController();
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
        ImGui::EndDisabled();
    }

    void drawRunPanel() {
        ImGui::SeparatorText("Run");
        if (!controller_) {
            ImGui::TextDisabled("Waiting for the toolchain.");
            return;
        }
        if (ImGui::Button("Merge files instead...")) {
            controller_->showMerge();
            // Seed the output folder from the split one: the user is almost always
            // splitting and re-joining into the same place, and an empty field that
            // must be filled before the button does anything is a needless step.
            if (mergeOutputBuffer_[0] == '\0') {
                std::snprintf(mergeOutputBuffer_, sizeof(mergeOutputBuffer_), "%s",
                              controller_->outputDirectory().c_str());
            }
        }
        helpMarker(HelpContent::find(HelpTopicId::Merge), [this](HelpTopicId id) { controller_->showHelp(id); });
        const bool running = controller_->jobRunning();
        const ConstraintReport report = controller_->validate();
        const bool canStart = report.valid && !running && controller_->selected() != nullptr;
        const float buttonWidth = std::max(80.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f);

        ImGui::BeginDisabled(running || controller_->selected() == nullptr);
        if (ImGui::Button("Preview plan", ImVec2(buttonWidth, 0.0f))) {
            controller_->previewPlan(controller_->outputDirectory());
        }
        helpMarker(HelpContent::find(HelpTopicId::Boundaries), [this](HelpTopicId id) { controller_->showHelp(id); });
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button("Start", ImVec2(buttonWidth, 0.0f))) {
            controller_->start(controller_->outputDirectory());
        }
        ImGui::EndDisabled();
        if (running) {
            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f))) {
                controller_->requestCancel();
            }
            ImGui::SameLine();
            if (controller_->snapshot().phase == SessionPhase::Paused) {
                if (ImGui::Button("Resume", ImVec2(buttonWidth, 0.0f))) {
                    controller_->requestResume();
                }
            } else if (ImGui::Button("Pause after this part", ImVec2(buttonWidth, 0.0f))) {
                controller_->requestPause();
            }
        }

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
        for (const auto& warning : snapshot.warnings) {
            ImGui::PushStyleColor(ImGuiCol_Text, toneColor(StatusTone::Warning));
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
        }
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
                // Through wideFromUtf8 rather than path(const std::string&), which
                // decodes UTF-8 bytes as the ANSI code page and hands back mojibake -
                // so a part written as "video été.mp4" was listed here under a name
                // that did not exist, while the file on disk was correct.
                ImGui::TextUnformatted(
                    utf8FromPath(std::filesystem::path(wideFromUtf8(part.path)).filename()).c_str());
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

    /// Starts watching a folder and feeds whatever it finds into the normal pipeline.
///
/// The point of the abstraction is that the folder watcher is platform code with
/// real Win32 in it, while the window must be testable.  The application implements
/// this with engine/WatchFolder.hpp, which already handles the two things that make
/// this hard: a notification buffer that can overflow (so a periodic rescan covers
/// what was missed) and a synchronous system call that would otherwise hang
/// shutdown.  Neither the controller nor this file knows how any of that works.
class WatchFolderSource {
public:
    virtual ~WatchFolderSource() = default;
    /// Starts watching.  Returns a message saying why it could not, or empty on
    /// success.  Safe to call while already watching: the previous watch is
    /// replaced.
    [[nodiscard]] virtual std::string startWatch(const std::string& folder, double settlingSeconds) = 0;
    virtual void stopWatch() = 0;
    [[nodiscard]] virtual bool watching() const = 0;
    /// Files seen and not yet accepted, each with the reason it is waiting.  This
    /// is what makes the watcher explainable: a user who asks why their file has
    /// not been picked up gets the actual reason, not silence.
    [[nodiscard]] virtual std::vector<std::pair<std::string, std::string>> pending() const = 0;
};

/// Drives engine/WatchFolder for the window.
///
/// The callback runs on the watcher's thread, so it does the least possible: push
/// the path and return.  Probing or running a job there would block the watcher
/// from noticing the next arrival for as long as the job takes, which is exactly
/// the case a watch folder exists to handle - a folder being filled faster than
/// the work can be done.  The UI thread drains the queue each frame, which also
/// means a burst of arrivals stays visible instead of being silently absorbed.
class EngineWatchFolder final : public WatchFolderSource {
public:
    EngineWatchFolder(WatchFolder& watcher, std::mutex& queueMutex, std::vector<std::string>& accepted)
        : watcher_(watcher), queueMutex_(queueMutex), accepted_(accepted) {}

    std::string startWatch(const std::string& folder, double settlingSeconds) override {
        watcher_.setSettlingSeconds(settlingSeconds);
        return watcher_.start(folder, [this](const std::filesystem::path& file) {
            std::lock_guard<std::mutex> guard(queueMutex_);
            accepted_.push_back(utf8FromPath(file));
        });
    }

    void stopWatch() override { watcher_.stop(); }
    bool watching() const override { return watcher_.running(); }
    std::vector<std::pair<std::string, std::string>> pending() const override { return watcher_.pending(); }

private:
    WatchFolder& watcher_;
    std::mutex& queueMutex_;
    std::vector<std::string>& accepted_;
};

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
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (worker_.joinable() || !workerFinished_.load()) {
                return;   // already running: a second probe would answer the same question
            }
        }
        // Outside the lock.  Joining while holding it is not a deadlock today only
        // because the worker publishes its result before it sets workerFinished_, and
        // that ordering is one careless edit away from being one.
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
    /// Joins the worker if there is one, and blocks until the probe is finished.
    ///
    /// This used to return immediately when a probe was still running, on the
    /// reasoning that blocking would freeze the window.  But then a probe that was
    /// still running at destruction time left a joinable std::thread behind, and
    /// destroying that calls std::terminate - so closing the window during the
    /// several seconds the probe can take killed the process instead of closing it.
    ///
    /// The UI thread therefore never calls this while a probe is in flight (startProbe
    /// returns early in that case); only destruction does, where blocking is the
    /// correct and unavoidable trade.
    void join() {
        if (worker_.joinable()) {
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

/// Join several files into one, without re-encoding.
    ///
    /// The order of the list is the join order and is not cosmetic - a
    /// concatenation plays in the order given - so it is editable here rather than
    /// left to a file dialog that always returns alphabetical order.
    ///
    /// The panel refuses to start a merge before it has planned one, for the same
    /// reason the split panel does: a merge of files whose codecs differ fails
    /// seconds later, and a plan states that up front with the actual difference.
    void drawMergePanel() {
        MergeSession& merge = controller_->merge();
        const bool busy = controller_->mergeRunning() || controller_->jobRunning();
        ImGui::BeginDisabled(busy);

        ImGui::SeparatorText("Merge");
        if (ImGui::Button("Back to splitting")) {
            controller_->hideMerge();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Switches back to the split panel. Nothing is lost.");
            ImGui::EndTooltip();
        }

        ImGui::TextDisabled("Files are joined in this order, without re-encoding.");

        // The ids are copied before the loop.  removeMergeInput erases from the very
        // vector this loop was iterating, so clicking "remove" on row 0 of three
        // invalidated the iterator and shifted every later index - the loop then
        // visited the row that had been third and skipped the one that had been second.
        // Indexing by position into a list that the loop itself edits is the whole bug;
        // iterating a snapshot of the identities is the fix.
        const std::vector<std::string> orderedIds = merge.inputs;
        for (std::size_t i = 0; i < orderedIds.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(orderedIds[i].c_str());
            // The offset goes negative once the column is narrower than the buttons
            // need, and ImGui treats a negative SameLine offset as a real position, so
            // the buttons were drawn to the *left* of the name and outside the panel.
            // The column is now draggable, which makes that trivial to reach.
            const float buttons = 150.0f;
            if (ImGui::GetContentRegionAvail().x > buttons) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttons);
            } else {
                ImGui::SameLine();
                ImGui::NewLine();
            }
            if (ImGui::SmallButton("up") && i > 0) {
                controller_->moveMergeInput(i, -1);
            }
            if (ImGui::SmallButton("down")) {
                controller_->moveMergeInput(i, 1);
            }
            if (ImGui::SmallButton("remove")) {
                controller_->removeMergeInput(i);
            }
            ImGui::PopID();
        }
        if (merge.inputs.size() < 2) {
            ImGui::TextDisabled("A merge needs at least two files.");
        }

        if (ImGui::Button("Add files...")) {
            for (const std::string& path : openFileDialogMulti(L"Choose files to merge")) {
                controller_->addMergeInput(path);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear") && !controller_->mergeRunning()) {
            controller_->clearMergeInputs();
        }

        ImGui::SeparatorText("Output");
        ImGui::TextUnformatted("Folder:");
        ImGui::SetNextItemWidth(-1.0f);
        // Written back only when the field actually changed, or when the folder button
        // below chose one.  Writing every frame meant the buffer was the single source
        // of truth and silently undid any other code path that set these values - and
        // the "Choose a folder..." result, assigned to the buffer here, did not reach
        // the controller until the next frame drew.
        const bool typedFolder = ImGui::InputText("##mergeout", mergeOutputBuffer_, sizeof(mergeOutputBuffer_));
        ImGui::TextUnformatted("File name:");
        ImGui::SetNextItemWidth(-1.0f);
        const bool typedName = ImGui::InputText("##mergename", mergeNameBuffer_, sizeof(mergeNameBuffer_));
        if (typedFolder || typedName) {
            merge.outputDirectory = mergeOutputBuffer_;
            merge.outputName = mergeNameBuffer_;
            merge.planned = false;
            merge.planSummary.clear();
        }
        if (ImGui::Button("Choose a folder...")) {
            const std::string folder = chooseFolderDialog(L"Write the merged file here", nativeWindow());
            if (!folder.empty()) {
                std::snprintf(mergeOutputBuffer_, sizeof(mergeOutputBuffer_), "%s", folder.c_str());
                // Pushed immediately rather than waiting for the next frame's typedFolder,
                // which would never be true for a value the user did not type.
                merge.outputDirectory = mergeOutputBuffer_;
                merge.planned = false;
                merge.planSummary.clear();
            }
        }

        if (!merge.planProblem.empty()) {
            ImGui::TextColored(toneColor(StatusTone::Warning), "%s", merge.planProblem.c_str());
        }
        if (merge.planned && !merge.planSummary.empty()) {
            ImGui::SeparatorText("Plan");
            ImGui::TextWrapped("%s", merge.planSummary.c_str());
        }

        ImGui::EndDisabled();
        if (controller_->mergeRunning()) {
            ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f), merge.headline.c_str());
            if (ImGui::Button("Cancel the merge")) {
                controller_->requestMergeCancel();
            }
        } else {
            ImGui::BeginDisabled(busy || merge.inputs.size() < 2 || merge.outputDirectory.empty());
            if (ImGui::Button("Preview")) {
                controller_->previewMerge(mergeOutputBuffer_, mergeNameBuffer_);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!merge.planned);
            if (ImGui::Button("Join the files") && !controller_->jobRunning()) {
                controller_->startMerge();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }

        if (!merge.message.empty()) {
            ImGui::SeparatorText("Result");
            ImGui::TextWrapped("%s", merge.message.c_str());
            if (merge.succeeded && !merge.path.empty()) {
                ImGui::Text("Verified: %s", merge.verified ? "yes" : "no");
                if (ImGui::Button("Open the output folder")) {
                    // Built from the UTF-8 bytes, not through path(const std::string&),
                    // which would decode them as the ANSI code page and mangle any path
                    // outside ASCII - the very filenames this product guarantees to
                    // handle.
                    const std::filesystem::path folder =
                        std::filesystem::path(wideFromUtf8(merge.path)).parent_path();
                    if (!folder.empty()) {
                        ::ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr,
                                        SW_SHOWNORMAL);
                    }
                }
            }
        }
        for (const std::string& warning : merge.warnings) {
            ImGui::TextColored(toneColor(StatusTone::Warning), "%s", warning.c_str());
        }
    }

    /// Watch a folder and ingest whatever lands in it.
    ///
    /// Two things this panel is careful not to do.  It does not claim a file is
    /// being watched before the watcher is running, and it never shows a file as
    /// "waiting" without saying why - the readiness rules can hold a file for a
    /// long time, and silence there reads as a broken product.
    ///
    /// Files are queued by the watcher thread and drained here, so a folder being
    /// filled faster than the work can be done stays visible instead of stalling.
    void drawWatchPanel() {
        ImGui::TextUnformatted("Watch a folder");
        helpMarker(HelpContent::find(HelpTopicId::WatchFolder), [this](HelpTopicId id) { controller_->showHelp(id); });

        if (ImGui::Button("Choose a folder to watch...")) {
            const std::string folder = chooseFolderDialog(L"Watch this folder for media files", nativeWindow());
            if (!folder.empty()) {
                watchFolder_ = folder;
                // The buffer is the widget's own storage, so the visible field has to be
                // updated here or the next frame's write-back would overwrite the choice
                // with the old text.
                std::snprintf(watchFolderBuffer_, sizeof(watchFolderBuffer_), "%s", folder.c_str());
                if (watchEnabled_) {
                    applyWatchSettings();
                }
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        // Written back only when the field changed, for the same reason as the merge
        // panel's fields: a buffer written every frame overrides anything else that
        // sets this value, and a folder chosen with the button on the next line would
        // otherwise take a frame to arrive.
        const bool typedWatchFolder = ImGui::InputText("##watchfolder", watchFolderBuffer_, sizeof(watchFolderBuffer_));
        if (typedWatchFolder) {
            // The edit field is a char buffer because that is what ImGui wants; the
            // watcher takes a std::string, so the conversion is explicit and happens
            // here rather than through a cast somewhere less visible.
            watchFolder_ = watchFolderBuffer_;
            if (watchEnabled_) {
                applyWatchSettings();
            }
        }

        if (ImGui::Checkbox("Watch this folder and split what appears", &watchEnabled_)) {
            applyWatchSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextUnformatted(
                "Each file is added to the queue once it is finished being written. MonoSect decides that from "
                "three independent signals - the file can be opened exclusively, its size has stopped changing, "
                "and its first bytes are readable - so a slow copy is waited for rather than split in half.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        if (ImGui::Checkbox("Wait longer before accepting a file", &watchSettling_)) {
            applyWatchSettings();
        }
        if (watchSettling_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::SliderFloat("##settling", &watchSettlingSlider_, 0.5f, 30.0f, "%.1f s")) {
                watchSettlingSeconds_ = watchSettlingSlider_;
                applyWatchSettings();
            }
        }

        if (!watchProblem_.empty()) {
            ImGui::TextColored(toneColor(StatusTone::Warning), "%s", watchProblem_.c_str());
        }
        if (watchSource_ == nullptr || !watchSource_->watching()) {
            if (!watchProblem_.empty() || !watchFolder_.empty()) {
                ImGui::TextDisabled("Not watching.");
            }
            return;
        }

        ImGui::Text("Watching %s", watchFolder_.c_str());
        // The pending list is the answer to "why has my file not been picked up
        // yet", so it is shown rather than summarised away.
        const std::vector<std::pair<std::string, std::string>> pending = watchSource_->pending();
        if (pending.empty()) {
            ImGui::TextDisabled("Nothing waiting.");
        }
        for (const auto& [name, reason] : pending) {
            ImGui::Text("%s", name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", reason.c_str());
        }
    }

    /// Starts or stops the watcher to match the panel's settings.
    ///
    /// Applied through one function because the settings are interdependent: turning
    /// the folder off must stop the watch, and changing the settling interval while
    /// watching must take effect immediately rather than at the next restart.
    void applyWatchSettings() {
        if (watchSource_ == nullptr) {
            return;
        }
        if (!watchEnabled_ || watchFolder_[0] == '\0') {
            watchSource_->stopWatch();
            watchProblem_.clear();
            return;
        }
        watchProblem_ = watchSource_->startWatch(watchFolder_, watchSettlingSeconds_);
    }

    /// Moves files the watcher accepted into the queue, on the UI thread.
    ///
    /// Called once per frame.  The swap under the lock is what keeps the watcher's
    /// callback short: it only ever appends, and this takes the whole list at once.
    void drainWatchedFiles() {
        std::vector<std::string> arrived;
        {
            std::lock_guard<std::mutex> guard(watchQueueMutex_);
            arrived.swap(watchedFiles_);
        }
        for (const std::string& path : arrived) {
            // A job already running must not be pre-empted by a watched file: the
            // user asked for one job, and silently starting another would leave the
            // first one's parts in an unknown state.  The file stays queued, so it is
            // picked up when the user starts the next one.
            if (controller_->jobRunning()) {
                controller_->addSource(path);
                continue;
            }
            controller_->addSource(path);
        }
    }

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
        // The list, then a verdict, then - only if asked - the full paragraph.
        //
        // The verdict is on one line because that is the only part anyone acts on: the
        // panel used to end in describe(), which is a paragraph naming every encoder
        // that failed and why, in full, unprompted, above two checkboxes.  The detail
        // is not deleted - it is one click away, and each encoder's own failure reason
        // was always a hover away.
        std::size_t usable = 0;
        std::string usableNames;
        for (const EncoderProbe& encoder : controller_->hardwareEncoders()) {
            if (!encoder.usable) {
                continue;
            }
            ++usable;
            if (!usableNames.empty()) {
                usableNames.append(", ");
            }
            usableNames.append(encoder.encoder);
        }
        const std::size_t tested = controller_->hardwareEncoders().size();
        if (usable == 0) {
            ImGui::TextDisabled("%zu tested - none work on this machine", tested);
        } else {
            ImGui::Text("%zu of %zu usable: %s", usable, tested, usableNames.c_str());
        }

        if (ImGui::CollapsingHeader("Why")) {
            ImGui::TextWrapped("%s", controller_->hardwareSummary().c_str());
        }
        helpMarker(HelpContent::find(HelpTopicId::GpuEncoders), [this](HelpTopicId id) { controller_->showHelp(id); });

        ImGui::TextWrapped("Hardware probes are informational. Splitting uses stream copy or software encoders; GPU acceleration is not enabled for jobs.");
    }

    void drawResourcesPanel() {
        ImGui::SeparatorText("Machine");
        if (!controller_) {
            return;
        }
        const ResourceSample& sample = controller_->resourceSnapshot();
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

        // The GPU gets the adapter's name on the same line as the figure, because the
        // question this panel gets asked is "what GPU has this machine got" and the
        // answer used to be "none" on a machine that had one.  When the percentage
        // cannot be read the name still stands, and the reason is on hover - an
        // unmeasurable figure and an absent device are different facts.
        {
            const std::string gpuName = controller_->gpuAdapterDescription();
            const std::string gpuValue = sample.gpuPercent.known
                                             ? UiModel::meterText(sample.gpuPercent, "%", 0)
                                             : std::string("n/a");
            ImGui::Text("GPU: %s - %s", gpuName.c_str(), gpuValue.c_str());
            if (!sample.gpuPercent.known && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", sample.gpuPercent.unavailableReason.c_str());
            }
            if (!gpuHistory_.empty()) {
                ImGui::PlotLines("##GPU", gpuHistory_.data(), static_cast<int>(gpuHistory_.size()), 0,
                                 nullptr, 0.0f, 100.0f, ImVec2(-1.0f, 42.0f));
            }
        }

        ImGui::Text("Disk read: %s", bytesPerSecondText(sample.diskReadBytesPerSecond).c_str());
        ImGui::Text("Disk write: %s", bytesPerSecondText(sample.diskWriteBytesPerSecond).c_str());
        ImGui::Text("Media written: %s", bytesPerSecondText(sample.mediaThroughputBytesPerSecond).c_str());
        // Closed by default.  It is two sentences of provenance, which is worth having
        // and is not worth two lines of panel for something nobody reads while working.
        if (ImGui::CollapsingHeader("Where these numbers come from")) {
            ImGui::TextWrapped("%s", sample.sourceDescription.c_str());
        }
        ImGui::Separator();

        drawWatchPanel();
        ImGui::Separator();

        drawHardwarePanel();

        ImGui::Separator();
        ResourcePolicyInput input = controller_->resourceInput();
        const ResourcePolicy policy = deriveResourcePolicy(input);
        helpMarker(HelpContent::find(HelpTopicId::ResourceModes), [this](HelpTopicId id) { controller_->showHelp(id); });
        // Same reasoning as the provenance paragraph above: this is a paragraph, and a
        // paragraph in a 380 px column is a wall.  It describes a policy the user chose
        // two controls below, so it belongs behind a disclosure rather than above them.
        if (ImGui::CollapsingHeader("What this schedule does")) {
            ImGui::TextWrapped("%s", UiModel::resourcePolicySummary(policy).c_str());
        }
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

    /// "2026-10-04T09:18:32" -> "10-04 09:18".
    ///
    /// The stored form is a full ISO timestamp and the right-hand column has four of
    /// them competing for width, where every character spent on the year and the
    /// seconds is a character not spent on the filename - which is the column the user
    /// is actually reading.  A job history is a list of recent jobs, so the year is
    /// the part that carries the least; it stays in the tooltip and in the record.
    /// Anything that does not match the ISO shape is passed through untouched rather
    /// than mangled.
    static std::string compactTimestamp(const std::string& stamp) {
        if (stamp.size() < 16 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != 'T') {
            return stamp;
        }
        std::string out;
        out.reserve(11);
        out.append(stamp, 5, 5);   // MM-DD
        out.push_back(' ');
        out.append(stamp, 11, 5);  // HH:MM
        return out;
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

        // Proportional columns, and a header that says so.
        //
        // SizingStretchProp with four unweighted columns gave every column a quarter of
        // the width: a 19-character timestamp got the same space as a filename, and the
        // filename - the only column with information in it - was the one that ran out.
        // That is what "disconnected" looked like on screen: the source names were cut
        // mid-word while half the row sat empty.
        //
        // Weights are 3.2 for the name against 1.6 and 1.2 for the rest, so the name
        // gets the room.  SizingStretchSame would share the surplus evenly, which
        // reproduces the original problem in a narrower column.
        //
        // ScrollY needs a bounded height.  An unbounded ScrollY table takes everything
        // left in the column, which would push the "Stored at" footer off the bottom and
        // make the panel scroll as a whole - the panel is a child, and a child that
        // scrolls as a whole is the thing that was already annoying.
        const float footerHeight = ImGui::GetTextLineHeightWithSpacing();
        const float tableHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y - footerHeight);
        // Weights are chosen so each column is wide enough for its own content at the
        // column's default width, because a stretch layout distributes slack
        // proportionally and will happily leave a column one character short:
        // "10-04 13:18" needs about 72 px, "Completed" about 70, the part count about
        // 46, and the filename - the column being read - takes everything left over.
        if (ImGui::BeginTable("##history", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0.0f, tableHeight))) {
            ImGui::TableSetupColumn("finished", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch, 3.6f);
            ImGui::TableSetupColumn("parts", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 1.4f);
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
                const std::string when = compactTimestamp(record.finishedAt);
                ImGui::TextUnformatted(when.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", record.finishedAt.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                // Wide char -> path -> narrow char on a UTF-8 std::string: the path
                // constructor decodes those bytes as the ANSI code page, so a filename
                // with anything outside ASCII came back as mojibake in this column.
                // Utf8Path is the project's boundary for exactly this, and the row's
                // tooltip has always had the correct full path underneath.
                const std::string name =
                    utf8FromPath(std::filesystem::path(wideFromUtf8(record.sourcePath)).filename());
                ImGui::TextUnformatted(name.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", record.sourcePath.c_str());
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(std::to_string(record.parts.size()).c_str());
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
            // Copied, not referenced.  helpResults() hands back a reference into the
            // controller, and showHelp() - called from inside this very loop - clears
            // that vector.  The stored end iterator then pointed into a vector that had
            // just been emptied, and the user-visible consequence was that clicking a
            // search result wiped the result list while the search box still showed the
            // query: a list that disappeared because it had been used.
            const std::vector<const HelpTopic*> results =
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
        if (ImGui::Begin("About MonoSect", &showAbout_)) {            ImGui::Text("MonoSect %s", version_.c_str());
            ImGui::TextDisabled("Precision media segmentation for Windows 10 and 11.");
            ImGui::Separator();
            ImGui::TextWrapped(
                "MonoSect splits a media file into parts that never exceed the limits you set. The limits are hard: "
                "a part is never published above the size in bytes or the duration you asked for, and every written "
                "part is verified before it is reported as finished.");
            ImGui::Separator();
            // Copied under the lock: these are written by the bootstrap thread and by
            // the manual locate handler, and an About panel is exactly the place a user
            // opens while the download is still running.
            std::string ffmpegShown;
            std::string ffprobeShown;
            {
                std::lock_guard<std::mutex> lock(bootstrapMutex_);
                ffmpegShown = ffmpegPath_;
                ffprobeShown = ffprobePath_;
            }
            ImGui::Text("FFmpeg: %s", ffmpegShown.empty() ? "not ready" : ffmpegShown.c_str());
            ImGui::Text("FFprobe: %s", ffprobeShown.empty() ? "not ready" : ffprobeShown.c_str());
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
    bool comInitialized_ = false;

    // --- column widths -----------------------------------------------------------------
    // Draggable, so a user whose constraints panel is wider than 420 px - or whose job
    // history needs more than 340 px - can make the column they need without resizing
    // the whole window.  Held on the window rather than baked into the layout because
    // ImGui offers no split view here: the three columns are sibling children, and a
    // child cannot be resized by dragging its own edge.
    static constexpr float kSplitterWidth = 7.0f;
    static constexpr float kMinLeftWidth = 220.0f;
    static constexpr float kMinRightWidth = 260.0f;
    static constexpr float kMinMiddleWidth = 180.0f;
    float leftWidth_ = 420.0f;
    float rightWidth_ = 380.0f;

    // Drag state for the two handles.  Tracked per window rather than per handle
    // because only one handle can be held at a time, and because the drag has to
    // survive the cursor leaving the gutter between the press and the release.
    bool splitterDragging_ = false;
    float splitterStartX_ = 0.0f;
    float splitterStartWidth_ = 0.0f;
    bool imguiReady_ = false;
    std::string version_;
    std::filesystem::path executableDirectory_;

    std::unique_ptr<FFprobeService> prober_;
    std::unique_ptr<EngineUiEngine> engine_;
    std::unique_ptr<UiController> controller_;
    /// The logo, loaded once the GL context exists.  Absent is fine; the window
    /// draws without it.
    vsm::TextureHandle logoTexture_;
    std::unique_ptr<BackgroundHardwareProbe> hardware_;
    std::unique_ptr<WatchFolder> watcher_;
    std::unique_ptr<WatchFolderSource> watchSource_;
    /// Written on the watcher's thread, read on the UI thread.
    std::mutex watchQueueMutex_;
    std::vector<std::string> watchedFiles_;
    /// The watched folder as UTF-8, which is what the watcher takes.  The buffer
    /// beside it is what ImGui edits, so the conversion is explicit rather than
    /// implicit in a cast.
    std::string watchFolder_;
    bool watchEnabled_ = false;
    bool watchSettling_ = false;
    double watchSettlingSeconds_ = 2.0;
    // ImGui's float slider needs a float; the watcher takes a double.  They are kept
    // as two members rather than casting at the call site so the value the user is
    // dragging and the value the watcher is given cannot drift apart.
    float watchSettlingSlider_ = 2.0f;
    std::string watchProblem_;

    std::jthread bootstrapThread_;
    std::atomic<bool> toolsReady_{false};
    std::atomic<bool> bootstrapRunning_{false};
    std::string bootstrapError_;
    std::vector<std::string> bootstrapLog_;
    std::mutex bootstrapMutex_;
    std::string ffmpegPath_;
    std::string ffprobePath_;
    std::vector<std::string> logLines_;

    // Paths are held in 32768-byte buffers, not 512.  A 512-byte buffer is enough for
    // MAX_PATH and the folder dialog will happily fill it - but every one of these is
    // written with snprintf whose return value nobody checked, so a deeper path was
    // silently truncated and the truncated string was then handed to setOutputDirectory
    // as a folder that does not exist.  The failure showed up much later, as parts that
    // could not be written, with the path in the message a prefix of the real one.
    // 32768 is what the Win32 extended-length limit allows and what executableDirectory()
    // already uses for the same reason.
    static constexpr std::size_t kPathBufferSize = 32768;
    char outputDirectoryBuffer_[kPathBufferSize] = {};
    char namingBuffer_[256] = {};
    char sizeValueBuffer_[64] = {};
    char durationValueBuffer_[64] = {};
    char databaseBuffer_[kPathBufferSize] = {};
    // Long enough for MAX_PATH, which is what the folder dialog fills it with.
    char watchFolderBuffer_[kPathBufferSize] = {};
    char mergeOutputBuffer_[kPathBufferSize] = {};
    char mergeNameBuffer_[256] = {};
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

int runApplicationWindow(std::string appVersion, bool smokeTest) {
    MainWindow window(appVersion.empty() ? std::string(MONOSECT_VERSION_STR) : std::move(appVersion));
    if (!window.initialize(smokeTest)) {
        if (!smokeTest) {
            MessageBoxW(nullptr, L"MonoSect could not open its window.", L"MonoSect", MB_ICONERROR | MB_OK);
        }
        return 1;
    }
    return window.run(smokeTest);
}

} // namespace monosect::ui
