// MonoSect — the application window (Windows only).
//
// This file draws.  Every decision it makes is a call into UiController, which is
// platform-neutral and unit-tested on the development machine
// (tests/core/UiControllerTests.cpp).  The rule that keeps the interface honest is
// that no string is composed here: status text, limit explanations, progress and
// verification wording all come from UiModel, and the tooltips come from
// HelpContent.  A drawing bug can look wrong; it cannot claim that a part was
// verified when it was not.
//
// Windows-only: this translation unit uses GLFW + OpenGL 3.3 + ImGui and the
// Win32 common dialogs.  It is built only by the `MonoSect` target, which the
// CMake configuration refuses to configure on a compiler that is not MinGW-w64
// g++ or MSVC.  It cannot be compiled or tested in a Linux environment; see
// docs/RESEARCH.md ("Windows-only components") and the final report.
#pragma once

#include <string>

namespace monosect::ui {

/// Opens the window, runs the message/draw loop and returns the process exit
/// code (0 = the window closed normally).
[[nodiscard]] int runApplicationWindow(std::string appVersion);

} // namespace monosect::ui
