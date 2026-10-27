// MonoSect — the Windows entry point.
//
// The window and every panel live in src/ui/MainWindow.cpp; the state machine it
// draws lives in src/ui/UiController.cpp, which is platform-neutral and tested on
// the development machine.
#include "core/Utf8Path.hpp"
#include "ui/MainWindow.hpp"

#include <windows.h>

#include <exception>
#include <string>

namespace {

/// Runs the window, turning any escaping exception into a message instead of a
/// crash dialog.
///
/// Everything below the UI is written in terms of std::filesystem, std::string
/// and the standard containers, so an allocation failure or a length_error from
/// an absurdly long path can in principle reach here.  The default handler for
/// that is std::terminate, which pops up Microsoft's "abnormal program
/// termination" box and offers to report the fault to Microsoft - the least
/// useful thing that can happen to a user in the middle of a job.  A product
/// that promises never to lose or corrupt a file must not die that way.
int runWithReporting(const std::string& version, bool smokeTest) {
    int result = 1;
    try {
        result = monosect::ui::runApplicationWindow(version, smokeTest);
    } catch (const std::exception& failure) {
        if (smokeTest) {
            return 1;
        }
        // failure.what() is UTF-8; widening it byte at a time would show the user
        // "0xc3 0xa9" instead of "é", so it goes through the one conversion that
        // does it properly.
        MessageBoxW(nullptr,
                    (std::wstring(L"MonoSect stopped because of an unexpected error:\n\n  ") +
                     monosect::core::wideFromUtf8(failure.what()) +
                     L"\n\nThe job record in the output folder lists what was already published.")
                        .c_str(),
                    L"MonoSect", MB_ICONERROR | MB_OK);
        return 1;
    } catch (...) {
        if (smokeTest) {
            return 1;
        }
        MessageBoxW(nullptr,
                    L"MonoSect stopped because of an unexpected error.\n\n"
                    L"The job record in the output folder lists what was already published.",
                    L"MonoSect", MB_ICONERROR | MB_OK);
        return 1;
    }
    if (result != 0 && !smokeTest) {
        MessageBoxW(nullptr, L"MonoSect could not start. See the log in the output folder for details.", L"MonoSect",
                    MB_ICONERROR | MB_OK);
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    // The version string is baked in by CMake so the title bar, the about box and
    // the job records always agree.
#ifdef MONOSECT_VERSION_STR
    const std::string version = MONOSECT_VERSION_STR;
#else
    const std::string version;
#endif
    const auto arguments = monosect::core::utf8CommandLineArguments(argc, argv);
    const bool smokeTest = arguments.size() == 2 && arguments[1] == "--smoke-test";
    return runWithReporting(version, smokeTest);
}
