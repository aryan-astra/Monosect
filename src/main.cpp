#include "ui/App.hpp"

#include <windows.h>

int main() {
    vsm::App app;
    if (!app.initialize()) {
        MessageBoxW(nullptr, L"Failed to initialize application", L"Monosect", MB_ICONERROR | MB_OK);
        return 1;
    }

    app.run();
    return 0;
}
