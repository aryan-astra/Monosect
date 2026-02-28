<p align="center">
  <img src="Monosect-full-logo.png" alt="Monosect Logo" width="600"/>
</p>

# Monosect

<p align="center"><em>Monolithic precision. Deterministic segmentation.</em></p>

A production-grade, GPU-aware desktop video split and merge application for Windows. Built with C++23, Dear ImGui, and FFmpeg — packaged as a single portable executable that auto-downloads its own dependencies on first run.

---

## Table of Contents

- [Features](#features)
- [System Requirements](#system-requirements)
- [Quick Start](#quick-start)
- [Build from Source](#build-from-source)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Technical Details](#technical-details)
- [Known Limitations](#known-limitations)
- [Branding](#branding)
- [License](#license)

---

## Features

- **5 split modes**: exact time, exact size, half-by-time, half-by-size, torrent preset (4 GiB)
- **3 precision modes**: strict lossless (stream-copy), exact frame boundary (re-encode at cuts), hybrid (GOP-aligned lossless with frame-accurate fallback)
- **Lossless merge**: concatenate segments without re-encoding via FFmpeg concat demuxer
- **GPU-accelerated encoding**: auto-detects NVENC (NVIDIA), AMF (AMD), QSV (Intel) hardware encoders
- **Adaptive binary search**: `SizePrecisionEngine` iteratively converges on exact target file sizes (within ±1% tolerance, max 8 iterations)
- **GOP analysis**: parses keyframe intervals to make intelligent split-point decisions
- **Watch folder**: monitors a directory for new media files via `ReadDirectoryChangesW` with 500ms debounce
- **Drag & drop**: drop files directly onto the window; validated for supported extensions (`.mp4`, `.mkv`, `.mov`, `.avi`, `.flv`, `.webm`)
- **Real-time system monitoring**: CPU usage (sampled via `GetSystemTimes`), throughput, and ETA — rendered as scrolling 300-sample plots
- **Thread pool**: concurrent job processing with `std::jthread` and `std::stop_token` for clean shutdown
- **Monochrome UI**: pure black/white/gray theme — no color accents anywhere
- **Portable**: single `.exe` + 2 PNG assets. FFmpeg downloaded automatically on first launch
- **Custom taskbar icon**: multi-resolution `.ico` (16/32/48/64/256px) generated at build time from source PNG

---

## System Requirements

| Component | Requirement |
|-----------|-------------|
| OS | Windows 10 64-bit or later |
| GPU | Any OpenGL 3.3 capable GPU (integrated or discrete) |
| RAM | 512 MB minimum |
| Disk | ~200 MB (for FFmpeg binaries after auto-download) |
| Network | Internet connection required on first launch (FFmpeg download) |
| Runtime | No additional runtime libraries required |

---

## Quick Start

### Pre-built

1. Download `Monosect.exe`, `Monosect-icon-square.png`, and `Monosect-full-logo.png`
2. Place all three files in the same folder
3. Double-click `Monosect.exe`
4. On first launch, the app automatically downloads FFmpeg (~70 MB) — progress is shown in the log panel
5. Add videos via drag & drop, the **Add Files** button, or enable the watch folder
6. Configure split mode, precision, and encoder — then click **Start Split**

### Distribution

To distribute the application, package these files together:

```
Monosect.exe                 # Main executable
Monosect-icon-square.png     # App icon (loaded at runtime)
Monosect-full-logo.png       # About dialog logo
```

FFmpeg binaries (`ffmpeg.exe`, `ffprobe.exe`) are automatically downloaded to the same directory on first run. No installer needed.

---

## Build from Source

### Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| **MinGW-w64** | GCC 13+ (tested with 15.1.0) | Must support C++23 (`-std=c++23`) |
| **CMake** | 3.24+ | Build system generator |
| **Ninja** | Any recent version | Build backend |
| **Git** | Any | For FetchContent to clone GLFW and ImGui |

> **Note**: MSVC is not supported. The build system explicitly rejects MSVC and targets MinGW-w64 only.

### Install MinGW via WinGet (recommended)

```powershell
winget install BrechtSanders.WinLibs.MCF.UCRT
```

### Build Commands

```powershell
# Configure (Release build with LTO)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build (adjust -j for your CPU core count)
cmake --build build --target Monosect -j 8

# Run
.\build\Monosect.exe
```

### Build Configuration

Release build compiler/linker flags:

| Flag | Purpose |
|------|---------|
| `-O3` | Maximum optimization |
| `-march=native` | Target current CPU architecture |
| `-flto` | Link-Time Optimization across all translation units |
| `-ffat-lto-objects` | Dual IR+object format (required for MinGW LTO with `gcc-ar`) |
| `-Wall -Wextra -Wpedantic` | Strict warnings |
| `-mwindows` | GUI subsystem (no console window) |

LTO uses `gcc-ar` and `gcc-ranlib` (auto-detected) instead of standard `ar`/`ranlib` to handle LTO object files correctly.

### Dependencies (auto-managed)

| Dependency | Source | Method |
|------------|--------|--------|
| **GLFW 3.4** | GitHub | CMake FetchContent |
| **Dear ImGui** (docking branch) | GitHub | CMake FetchContent |
| **OpenGL 3.3** | System | Linked via `-lopengl32` |
| **GDI+** | Windows SDK | Linked via `-lgdiplus` |
| **WinHTTP** | Windows SDK | Linked via `-lwinhttp` |
| **FFmpeg** | BtbN GitHub Releases | Auto-downloaded at runtime |

No vcpkg, Conan, or manual dependency installation required.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                         App (UI Layer)                       │
│  GLFW Window → OpenGL 3.3 → Dear ImGui (Docking Branch)     │
│  Panels: Queue │ Controls │ Metrics │ Log │ About            │
├──────────────────────────────────────────────────────────────┤
│                      Engine Layer                            │
│  SplitEngine ─── FFmpegProcess ──→ ffmpeg.exe / ffprobe.exe │
│       │              │                                       │
│       ├── GOPAnalyzer (keyframe interval analysis)           │
│       ├── MediaProbe (ffprobe JSON parsing)                  │
│       └── SizePrecisionEngine (adaptive binary search)       │
├──────────────────────────────────────────────────────────────┤
│                    Support Layer                             │
│  ThreadPool ── SystemMonitor ── HardwareDetector             │
│  WatchFolder ── FFmpegBootstrap ── Branding                  │
└──────────────────────────────────────────────────────────────┘
```

### Data Flow

1. User adds files → validated for extension and path safety (no `..` traversal)
2. **MediaProbe** runs `ffprobe -print_format json` to extract duration, bitrate, resolution, FPS, audio/subtitle tracks
3. **GOPAnalyzer** runs `ffprobe -select_streams v:0 -show_entries packet=pts_time,dts_time,flags,pos` to map keyframe positions
4. **SplitEngine** computes split points based on the selected mode and precision
5. **FFmpegProcess** spawns `ffmpeg.exe` via `CreateProcessW` with pipe-based I/O and `CREATE_NO_WINDOW`
6. **ThreadPool** manages concurrent split jobs; results logged to the UI in real-time
7. **SystemMonitor** samples CPU via `GetSystemTimes` every 500ms; throughput calculated from split progress

---

## Project Structure

```
merge/
├── CMakeLists.txt                      # Build configuration (MinGW only)
├── README.md                           # This file
├── Monosect-icon-square.png            # 768×768 app icon source
├── Monosect-full-logo.png              # 1408×768 full logo for About dialog
├── resources/
│   ├── Monosect.rc                     # Resource script embedding .ico
│   └── Monosect.ico                    # Multi-resolution icon (auto-generated)
├── src/
│   ├── main.cpp                        # Entry point (WinMain)
│   ├── ui/
│   │   ├── App.hpp / App.cpp           # Main app class, ImGui docking layout
│   │   └── Branding.hpp / Branding.cpp # GDI+ PNG loading, texture upload
│   ├── engine/
│   │   ├── Types.hpp                   # Core data types, enums, validators
│   │   ├── ParseUtils.hpp              # Shared inline parse utilities
│   │   ├── FFmpegBootstrap.hpp / .cpp  # Auto-download FFmpeg on first run
│   │   ├── FFmpegProcess.hpp / .cpp    # Win32 CreateProcessW wrapper
│   │   ├── SplitEngine.hpp / .cpp      # Split/merge orchestration
│   │   ├── GOPAnalyzer.hpp / .cpp      # Keyframe interval analysis
│   │   ├── MediaProbe.hpp / .cpp       # ffprobe JSON metadata extraction
│   │   ├── ThreadPool.hpp / .cpp       # std::jthread concurrent executor
│   │   └── WatchFolder.hpp / .cpp      # ReadDirectoryChangesW file monitor
│   ├── precision/
│   │   └── SizePrecisionEngine.hpp/.cpp # Adaptive binary search for sizes
│   └── monitor/
│       ├── SystemMonitor.hpp / .cpp    # CPU sampling via GetSystemTimes
│       └── HardwareDetector.hpp / .cpp # GPU encoder detection
└── .vscode/
    ├── settings.json                   # Editor settings
    └── c_cpp_properties.json           # IntelliSense configuration
```

---

## Technical Details

### FFmpeg Auto-Bootstrap

On first launch, the application checks for `ffmpeg.exe` and `ffprobe.exe` next to the executable. If either is missing:

1. **Download**: WinHTTP downloads `ffmpeg-master-latest-win64-gpl.zip` (~70 MB) from `github.com/BtbN/FFmpeg-Builds/releases/download/latest/`
2. **HTTPS + Redirects**: Follows HTTP 301/302 redirects automatically (GitHub CDN → `objects.githubusercontent.com`)
3. **Extraction**: PowerShell `Expand-Archive` unzips; locates and copies `ffmpeg.exe` and `ffprobe.exe` from the nested `bin/` directory
4. **Cleanup**: Removes the temporary `.zip` file and extracted directory
5. **Fallback**: On failure, logs a warning — user can manually place the binaries

Progress messages are displayed in real-time in the log panel.

### Split Modes

| Mode | Description | FFmpeg Strategy |
|------|-------------|-----------------|
| **Exact Time** | Split at a specific timestamp (seconds) | `-ss` seek + `-t` duration |
| **Exact Size** | Split to produce segments of a target file size | Adaptive binary search over duration |
| **Half by Time** | Split at the temporal midpoint | Duration ÷ 2 |
| **Half by Size** | Split at the size midpoint | File size ÷ 2 → binary search |
| **Torrent Preset** | Split into ~4 GiB segments | Target = 4,294,967,296 bytes |

### Precision Modes

| Mode | Behavior | Trade-off |
|------|----------|-----------|
| **Strict Lossless** | Stream-copy only (`-c copy`), splits at nearest keyframe | Fast, zero quality loss, split points may not be frame-exact |
| **Exact Frame Boundary** | Re-encodes the few frames around cut points | Slower at cuts, but frame-accurate |
| **Hybrid** | GOP-aligned lossless when possible; re-encodes only between keyframes | Best balance of speed and accuracy |

### Adaptive Binary Search (SizePrecisionEngine)

For size-based splitting:

1. Estimate initial duration from `(target_bytes / bitrate) × 8`
2. Run FFmpeg with that duration → check actual output size
3. If output is too large, reduce duration; if too small, increase
4. Repeat until within ±1% of target or max 8 iterations

Handles variable bitrate content where simple arithmetic would be inaccurate.

### Hardware Acceleration

`HardwareDetector` probes FFmpeg for available hardware encoders by running `ffmpeg -hide_banner -encoders` and searching for:

- **NVENC**: `h264_nvenc`, `hevc_nvenc` (NVIDIA)
- **AMF**: `h264_amf`, `hevc_amf` (AMD)
- **QSV**: `h264_qsv`, `hevc_qsv` (Intel)

Detected encoders populate the UI dropdown. Fallback: `libx264` (software).

### Process Management

All FFmpeg/ffprobe invocations use `CreateProcessW` with:
- `CREATE_NO_WINDOW` flag (no console flash)
- Redirected `stdout`/`stderr` via anonymous pipes
- Proper handle inheritance and cleanup

### Thread Pool

C++20 `std::jthread` with cooperative cancellation via `std::stop_token`. Workers block on `std::condition_variable_any::wait` with integrated stop token — requesting stop automatically wakes all threads for clean shutdown.

### Watch Folder

Monitors a directory using `ReadDirectoryChangesW`:
- Watches for `FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE`
- 500ms debounce to avoid processing partially-written files
- Validates extension and path safety before queueing

### UI Rendering

- **GLFW** creates a native Win32 window + OpenGL 3.3 core context
- **Dear ImGui** (docking branch) provides the immediate-mode UI
- 3-panel dockable layout: Left (Queue), Right (Controls + Metrics), Bottom (Log)
- Menu bar renders first; dockspace uses `viewport->WorkPos` / `viewport->WorkSize`
- Pure monochrome theme: all `ImGuiCol_*` values are grayscale
- **Segoe UI** font at 16px with DPI-aware scaling (falls back to ImGui default)
- Branding: icon via GDI+ → OpenGL texture; `.ico` generated at build time via PowerShell

---

## Known Limitations

1. **Windows only** — uses Win32 APIs throughout (CreateProcessW, ReadDirectoryChangesW, GetSystemTimes, WinHTTP, GDI+)
2. **MinGW only** — build system rejects MSVC. Tested with GCC 15.1.0 (WinLibs MCF/UCRT)
3. **First-launch network** — FFmpeg must be downloaded on first run (~70 MB). Offline fallback: manually place `ffmpeg.exe` and `ffprobe.exe` next to the executable
4. **No GPU monitoring** — GPU usage in metrics is always 0%. Windows does not expose GPU utilization through a simple Win32 API
5. **Serial LTO** — MinGW's link-time optimization runs serially, causing ~30–60s link times
6. **`-march=native`** — built executable is optimized for the build machine's CPU and may not run on older hardware

---

## Branding

Developed under the **Monosect** brand.

- Square icon: `Monosect-icon-square.png` (768×768)
- Full logo: `Monosect-full-logo.png` (1408×768)

---

## License

All rights reserved. Proprietary software by Monosect.
