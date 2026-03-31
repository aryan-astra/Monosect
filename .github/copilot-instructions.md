# Copilot instructions for Monosect

## Build, test, and lint commands

### Build (Windows + MinGW-w64 only)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Monosect -j 8
.\build\Monosect.exe
```

Notes:
- This repo targets **Windows + MinGW-w64 g++**. `CMakeLists.txt` rejects non-Windows and rejects MSVC.
- `CMAKE_BUILD_TYPE` defaults to `Release`; non-Release builds show a warning.
- Build links statically and enables LTO (`-O3 -march=native -flto -ffat-lto-objects`).
- Build step generates `resources\Monosect.ico` from `Monosect-icon-square.png` via PowerShell.

### Tests

No automated test suite is configured in this repository:
- no `enable_testing()` / `add_test()` in CMake
- no test directories or test targets

Single-test command: **not available** (no test harness present).

### Lint

No lint target/tooling is configured (no clang-tidy/cppcheck/cpplint target in CMake).

## High-level architecture

- Entry point is `src\main.cpp`: creates `vsm::App`, calls `initialize()`, then `run()`.
- `App` (`src\ui\App.*`) is the orchestration layer: GLFW window, OpenGL 3.3, Dear ImGui docking UI, queue management, logs, watch-folder controls, and split job dispatch.
- FFmpeg bootstrap is asynchronous in `App::initialize()`:
  - `ensureFFmpeg()` downloads/extracts `ffmpeg.exe` + `ffprobe.exe` next to the executable if missing.
  - After bootstrap, `SplitEngine`, `MediaProbe`, and `HardwareDetector` are re-bound to local FFmpeg binaries.
- Split workflow:
  - UI builds `SplitRequest` (`App::makeRequest`).
  - `App::startSplitJobs()` enqueues work on `ThreadPool`.
  - `SplitEngine::split()` routes to time-based or size-based splitting.
- Time-based split (`SplitEngine::splitByTime`):
  - probes duration via `MediaProbe`
  - analyzes keyframes via `GOPAnalyzer`
  - splits with stream-copy FFmpeg commands at nearest keyframe.
- Size-based split (`SplitEngine::splitBySize`):
  - uses `SizePrecisionEngine` binary search with temporary estimate files
  - then executes final split via `splitByTime`.
- Support components:
  - `FFmpegProcess`: Win32 `CreateProcessW` wrapper with stdout/stderr pipe capture.
  - `SystemMonitor`: CPU sampling via `GetSystemTimes`; GPU metric currently reported as `0.0`.
  - `WatchFolder`: `ReadDirectoryChangesW` watcher with debounce before enqueueing files.

## Key conventions in this codebase

- Code is under `namespace vsm`.
- Media path safety is enforced with `isSafePath()` (`src\engine\Types.hpp`), which rejects `..` traversal components.
- Media intake is extension-gated with `isSupportedMediaExtension()` (`.mp4`, `.mkv`, `.mov`, `.avi`, `.flv`, `.webm`).
- Queue and logs are shared across UI + worker threads; use existing mutexes (`queueMutex_`, `logsMutex_`) for access.
- Logging convention is through `App::pushLog()`; logs are capped (old entries are trimmed after reaching 1000).
- `SplitEngine` currently uses stream-copy (`-c copy`) in actual split commands; `SplitRequest` fields like `precisionMode`, `useHardwareAcceleration`, and `unrestrictedMode` are populated by UI but not fully applied in splitting logic.
- `GOPAnalyzer::nearestKeyframe` currently chooses the nearest keyframe by absolute delta.
- Size precision uses `SizePrecisionEngine::findSplitTime()` with cached measurements keyed by rounded milliseconds.
- Size-based split creates temporary `*_estimate_*` files in output dir and removes them after search.
- Split output naming convention is fixed to `<stem>_part1<ext>` and `<stem>_part2<ext>`.
- Merge path (`SplitEngine::mergeLossless`) exists and validates codec/resolution/track-count compatibility before FFmpeg concat demuxer execution.
- FFmpeg/ffprobe subprocesses should go through `FFmpegProcess`; it handles quoting, UTF-8/UTF-16 conversion, hidden window mode, and pipe draining.
- Watch-folder events are debounced by size stability + elapsed time before callback emission.
