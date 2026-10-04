# Research basis

Every non-trivial subsystem of MonoSect is built on a primary source: the FFmpeg
documentation or source, a Microsoft API reference, the C++ standard, or the
documentation of the library that is used. This file records which source settles
which question, what was taken from it, and where MonoSect deliberately does
something *different* because a measurement contradicted the documentation or
because the documented behaviour is unsafe for this product.

Two rules were followed while writing this file:

* A blog post is never the reason for a decision. Where a well-known claim comes
  from a secondary source, the claim is marked as **unverified** and the decision
  rests on a measurement in this repository or on the primary source instead.
* Where MonoSect relies on a measurement against the documentation, the
  measurement is recorded here, because that is the interesting part.

---

## 1. FFmpeg: cutting and copying

| Question | Source | What was taken from it |
|---|---|---|
| What do `-ss`, `-to`, `-t` mean, before and after `-i`? | FFmpeg CLI documentation, `ffmpeg-all(1)` (https://ffmpeg.org/ffmpeg.html#Main-options, `-ss`, `-to`, `-t`, `-accurate_seek`) | `-ss` before `-i` seeks and, since FFmpeg 2.1, decodes to the exact position when the input is not copied; with `-c copy` it lands on the nearest keyframe at or before the requested time. After `-i` it discards decoded frames. MonoSect never relies on `-ss` alone for a hard boundary: boundaries are chosen against the packet index first (see `core/PacketIndex.hpp`). |
| How are timestamps handled when the pieces are copied out? | `-copyts`, `-start_at_zero`, `-avoid_negative_ts`, `-muxdelay`, FFmpeg CLI documentation | `-copyts` keeps input timestamps, `-start_at_zero` rebases them, and they must not be combined for this job (measured: audio first pts became 0.003000 with both). MonoSect copies with `-copyts` for the copied path and rebases audio explicitly where the container requires it. |
| How is an exactly-timed copy produced at all? | Measurement in this repository; no documentation states it | `-ss`/`-to` with `-c copy` do not give an exact trim: the output starts at a keyframe and the duration is a consequence. `-frames:v N` / `-frames:a N` with `-ss` **before** `-i` on the copy path produces the exact packet range that the packet index promised. This is what `engine/FfmpegArgs.cpp` emits, and `tests/harness/check_parts.py` verifies the result independently of MonoSect. |
| Which packet fields can be used to plan a cut? | `ffprobe(1)` (https://ffmpeg.org/ffprobe.html), `-show_packets`, `-show_entries packet=...`, `-of json`, `read_intervals` | Per-packet `pts_time`, `dts_time`, `duration_time`, `size`, `pos`, `flags`. Packets are returned in decode order, which is why `core/PacketIndex` builds a timestamp-ordered view per stream rather than trusting the order of the JSON array. |
| What is in the JSON probe output? | `ffprobe(1)`, `-show_format`, `-show_streams`, `-show_chapters`, `-print_format json` | The fields parsed by `core/ProbeParsing.cpp`. Anything absent is reported as unknown; nothing is defaulted to a plausible value. **Measured caveat:** the development ffprobe build (N-66595, see below) crashes with signal 11 when `-show_format`/`-show_streams` are combined with JSON output on an MPEG-TS fixture. That is a defect of that build, not of the command line; the harness skips that one check and says so. |
| Why drive the CLI instead of libavformat? | FFmpeg CLI documentation plus the licensing position in the FFmpeg legal page (https://ffmpeg.org/legal.html) | MonoSect ships the pinned tools and runs them; it does not link libav*, so the LGPL/GPL question for a linked library does not arise, and a user can replace the tools with their own build. `libavformat`'s API for "copy this packet range" is also a moving target across majors. |
| Which container does an extension mean? | FFmpeg muxer documentation (`ffmpeg -muxers`) | `.mkv` → `matroska`, `.mp4`/`.m4v` → `mp4`, `.m4a` → `ipod`, `.ts`/`.m2ts` → `mpegts`. This mapping is in `core/Constraints.cpp` (`containerNameForExtension`) because a part written with the wrong muxer is a different file, not a renamed one. |

**Contradiction, resolved by measurement.** Widely repeated advice says `-ss`
before `-i` makes `-to` relative to the seek point and that this yields the
requested duration. With `-c copy` it does not: the first packet is a keyframe at
or before the requested time, so parts come out longer than asked and the hard
duration limit can be broken. Measured in `tests/harness/end_to_end.sh` (section
`split-duration`). MonoSect therefore plans against the packet index and verifies
the written duration, and it never rounds a limit up.

---

## 2. Windows APIs

| Question | Source | What was taken from it |
|---|---|---|
| How does a child process get killed so that it cannot survive MonoSect? | Microsoft Learn, `CreateProcessW`, job objects, `JOBOBJECT_EXTENDED_LIMIT_INFORMATION`, `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `TerminateJobObject` (https://learn.microsoft.com/windows/win32/procthread/job-objects) | `engine/win/WindowsProcessRunner.cpp` creates the process suspended, assigns it to a job object with `KILL_ON_JOB_CLOSE`, then resumes it. If MonoSect crashes, the operating system kills FFmpeg; no orphan can hold a file lock. |
| How is CPU time measured? | Microsoft Learn, `GetSystemTimes` (https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes) | Idle/kernel/user are `FILETIME`s in 100 ns units; kernel time *includes* idle, so busy = kernel + user − idle. That subtraction is in `monitor/SystemMonitor.cpp` and is the usual source of wrong CPU graphs. |
| How many physical cores are there, and which are efficiency cores? | Microsoft Learn, `GetLogicalProcessorInformationEx`, `SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX`, `PROCESSOR_RELATIONSHIP`, `EfficiencyClass` (https://learn.microsoft.com/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformationex) | Physical cores are counted from `RelationProcessorCore` records rather than trusting `GetSystemInfo` (which reports logical processors). On hybrid CPUs the `EfficiencyClass` distinguishes performance from efficiency cores, which `core/ResourcePolicy.cpp` uses when it decides how many encoder threads to allow. |
| How much memory is in use? | Microsoft Learn, `GlobalMemoryStatusEx` / `MEMORYSTATUSEX.dwMemoryLoad` | The percentage the resource panel shows. |
| How is GPU utilization read without elevating? | NVIDIA NVML API reference, `nvmlDeviceGetUtilizationRates`, `nvmlInit_v2`, `nvmlDeviceGetHandleByIndex_v2` (https://docs.nvidia.com/deploy/nvml-api/) | `monitor/SystemMonitor.cpp` loads `nvml.dll` dynamically, so MonoSect runs unchanged on a machine without an NVIDIA GPU. NVML is the only telemetry interface implemented; the AMD and Intel equivalents report `n/a` with a reason rather than a number. |
| How is disk throughput measured? | Microsoft Learn, Performance Data Helper: `PdhOpenQuery`, `PdhAddEnglishCounter`, `PdhCollectQueryData`, `PdhGetFormattedCounterValue` (https://learn.microsoft.com/windows/win32/perfctrs/using-the-pdh-functions-to-consume-counter-data) | `\PhysicalDisk(_Total)\Disk Read Bytes/sec` and `...\Disk Write Bytes/sec`, sampled at 1 Hz. The first sample only establishes the baseline and is reported as unavailable, not as zero. |
| Why is hardware-accelerated encoding probed instead of trusted? | FFmpeg `ffmpeg -encoders` and the vendor encoder documentation (NVENC/QSV/AMF) plus the known behaviour that a listed encoder can still fail at runtime | `core/CapabilityMatrix` runs a one-frame encode to decide whether an encoder is *usable*, and the job history records the outcome. A listing in `-encoders` is not evidence that the GPU can encode this input. |
| Where do the pinned FFmpeg builds come from? | The FFmpeg download page (https://ffmpeg.org/download.html) and the BtbN Windows builds it links to; gyan.dev builds (https://www.gyan.dev/ffmpeg/builds/) are the alternative. | `core/DependencyPin.cpp` holds the address and the release name the product ships, and `engine/FFmpegBootstrap.cpp` refuses to run a tool it cannot identify: a pinned SHA-256 (CMake `MONOSECT_FFMPEG_SHA256`) must match the downloaded archive and the extracted binaries, and a build without a pinned hash records the hash of its first download and enforces it from then on. **Remaining release work:** the release process must fill in the pinned hash; until it does, a developer build is protected against later substitution but not against a first download that was already wrong. |

---

## 3. C++ and the standard library

| Question | Source | What was taken from it |
|---|---|---|
| How are exact sizes and durations represented? | C++ standard, `<cstdint>`; the arithmetics in `core/Rational.hpp` | Sizes are `std::int64_t` counts of bytes; times are exact rationals of `int64_t` pairs (frame grids are 1/12800, audio is 1/44100 and similar). No `double` takes part in a limit comparison anywhere: `compareRational` is the only comparison used for a limit. |
| Why `std::jthread` and `std::stop_token`? | C++20/23 standard, `<thread>`, `std::jthread` | Cooperative shutdown: the bootstrap and the auto-tuner take a stop token; the window joins them before exiting. |
| How are the working directories committed atomically? | C++ standard, `<filesystem>` (`rename`, `exists`, `space`), plus the platform rule that a rename within a volume is atomic | `engine/OutputCommitter.cpp` writes into `.monosect-<job>` on the destination volume and renames each finished part into place, so a part is either absent or complete. `std::filesystem::space` is checked before the job starts and a warning is logged if the drive is too small. |

---

## 4. UI frameworks

| Question | Source | What was taken from it |
|---|---|---|
| Why ImGui + GLFW + OpenGL and not a web view? | Dear ImGui documentation (https://github.com/ocornut/imgui/blob/master/docs/FAQ.md), GLFW documentation (https://www.glfw.org/documentation.html) | A native, self-contained Windows application with no browser runtime, no scripting layer and no network stack of its own. `CMakeLists.txt` pins ImGui v1.91.5 and GLFW 3.4 through `FetchContent`. The decision is recorded here because "rewrite it in Electron" was rejected for size, startup time and offline behaviour. |
| How does the window stay testable for a Linux developer? | ImGui FAQ ("How can I have multiple…", "How can I use ImGui in a unit test") — the library is not designed for headless tests | The window is split into `UiController` (platform-neutral state machine, unit-tested) and `MainWindow` (drawing only). No rule is implemented in the drawing code, which is also why the text of every limit, status and verification line comes from `UiModel`. |
| Why is `(?)` next to a control a help *link* rather than a paragraph? | ImGui's own behaviour (`ImGui::IsItemHovered`, tooltips) | Tooltips are short; the explanation lives in `HelpContent` so the same sentence is used by the tooltip, the help window and the tests that check the wording. |

---

## 5. Build and release

| Question | Source | What was taken from it |
|---|---|---|
| How are the third-party dependencies fetched reproducibly? | CMake documentation, `FetchContent` (https://cmake.org/cmake/help/latest/module/FetchContent.html) | Pinned tags (`GIT_TAG 3.4`, `GIT_TAG v1.91.5`) and `CMAKE_CONFIGURE_DEPENDS` on `version.txt`, so a version bump reconfigures. |
| Which compiler builds the GUI? | MinGW-w64 project documentation (https://www.mingw-w64.org/); the CMake configuration refuses MSVC with a message | The GUI target asks for `-march=x86-64-v2`, which is the SSE4.2/AVX baseline of every Windows 10 machine. |
| How is the version kept in one place? | CMake documentation, `project(... VERSION ...)` and `configure_file` | `version.txt` holds `MAJOR.MINOR.PATCH`; the window title, the about box and the job history all read the same value. |

---

## 6. What could not be verified in the development environment

The development machine is Linux. These items are implemented against the
sources above but **cannot be executed here**; each one is called out again in
the final report:

1. **Everything in `src/ui/MainWindow.cpp`** — it needs Windows, GLFW, ImGui and
   the Win32 dialogs. The rules it displays are tested (see
   `tests/core/UiControllerTests.cpp`); the drawing code is not compiled here.
2. **`engine/win/WindowsProcessRunner.cpp`** — job-object based process control.
   The POSIX runner is exercised by the harness; the Windows one is not.
3. **`engine/FFmpegBootstrap.cpp`** — the download and SHA-256 verification of the
   pinned builds, and the `gh`/https fallback order.
4. **NVML and PDH telemetry** — on Linux those metrics report `n/a` with the
   reason, which is the documented behaviour, but the Windows path that produces
   numbers is not exercised here.
5. **Hardware encoders (NVENC/QSV/AMF)** — the capability probe cannot run
   without the hardware.
6. **The release workflow** (`.github/workflows`) — it runs on GitHub's Windows
   runners.

## 7. Development environment note (not a product decision)

The Linux sandbox used to develop and verify the core has its own ffmpeg/ffprobe:
`ffmpeg` 7.0.2 (static, from the `imageio-ffmpeg` wheel) and `ffprobe` N-66595
(from the `@ffprobe-installer/linux-x64` package). The ffprobe build crashes on
some MPEG-TS probes, so `tests/harness/end_to_end.sh` skips exactly one check
with an explicit message naming the tool version. The product's own pinned
Windows builds are unaffected; the pinned versions and their checksums live in
`engine/FFmpegBootstrap.cpp`.
