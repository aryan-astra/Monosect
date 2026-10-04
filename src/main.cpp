// MonoSect — the Windows entry point.
//
// The window and every panel live in src/ui/MainWindow.cpp; the state machine it
// draws lives in src/ui/UiController.cpp, which is platform-neutral and tested on
// the development machine.
#include "ui/MainWindow.hpp"

#include <windows.h>

int main() {
    // The version string is baked in by CMake so the title bar, the about box and
    // the job records always agree.
#ifdef MONOSECT_VERSION_STR
    const std::string version = MONOSECT_VERSION_STR;
#else
    const std::string version;
#endif
    const int result = monosect::ui::runApplicationWindow(version);
    if (result != 0) {
        MessageBoxW(nullptr, L"MonoSect could not start. See the log in the output folder for details.", L"MonoSect",
                    MB_ICONERROR | MB_OK);
    }
    return result;
}
