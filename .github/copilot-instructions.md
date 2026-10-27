# Copilot instructions for Monosect

Everything below was checked against the repository. Where a claim could not be
verified it is marked, rather than repeated because it was in an earlier version of
this file.

## Build, test, and lint commands

### Build (Windows + MinGW-w64 only)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- The GUI targets **Windows + MinGW-w64 g++**. MSVC is refused for the GUI.
  The core, harness and tests also have a POSIX build; Windows-only process and
  watcher implementations are selected conditionally in CMake.
- `CMAKE_BUILD_TYPE` defaults to `Release`.
- The GUI target is `Monosect`; there are also `monosect-cli` (a headless harness that
  drives the same engine) and `monosect-tests`. `cmake --build build` builds all three.
- The release flags are `-O2 -march=x86-64-v2 -mtune=generic` on the GUI target.
  There is **no LTO**; an earlier version of this file claimed `-O3 -flto`, which
  was never true of this CMakeLists.

### Tests

There are two suites, and they answer different questions. Run both.

**Unit tests** - the rules that must never regress. Fast, no FFmpeg, no media:

```powershell
cmake --build build --target monosect-tests
.\build\monosect-tests.exe
```

**End-to-end suite** - the product. Drives the real CLI against real media in every
container MonoSect claims to support, and re-checks every output with ffprobe alone.
It needs the pinned FFmpeg binaries:

```powershell
$env:MONOSECT_FFMPEG  = "C:\path\to\ffmpeg.exe"
$env:MONOSECT_FFPROBE = "C:\path\to\ffprobe.exe"
python tests/harness/end_to_end.py build\monosect-cli.exe e2e-work
```

The end-to-end suite is the one that finds real defects. Every bug found during the
integration of the current engine was invisible to the unit tests, because each only
appears when FFmpeg is actually asked to split a real file:

- a stream copy whose last part failed on a timestamp-less "skip pre-roll" packet;
- a seek passed as an absolute presentation time where `-ss` expects a time relative
  to the container's `start_time`, which silently dropped the tail of a transport
  stream;
- attachments dropped from every part, because they have no timeline to trim;
- a re-encode that used the encoder's default codec profile, so the verifier rejected
  MonoSect's own output for not matching the source.

If you change anything the engine does with FFmpeg, run this suite. It is the only
thing standing between a plausible change and a broken product.

`python` is required and `bash` is not available on this platform - do not add shell
scripts. `end_to_end.py` takes `<cli-path> [work-directory]`; `MONOSECT_E2E_SKIP`
can skip cancellation and the run writes its verdict to `<work-dir>/e2e.log`.

### Lint

No lint target is configured. The build is warning-clean in project code; the warnings
that appear during a full rebuild come from `_deps` (GLFW, ImGui) and are not ours.

## High-level architecture

Read in this order - each layer only knows the one below it.

- **`src/core/`** - platform-neutral rules. Planning (`SplitPlanner`), constraints and
  their validation (`Constraints`), container model (`MediaModel`), packet index
  (`PacketIndex`), verification (`Verification`), size prediction (`SizeModel`),
  merge planning (`MergePlanner`), stream-capability matrix (`CapabilityMatrix`),
  watch-folder readiness rules (`WatchReadiness`), dependency pinning
  (`DependencyPin`), UTF-8 boundaries (`Utf8Path`). No FFmpeg, no Windows headers.
- **`src/engine/`** - everything that runs FFmpeg. `FFprobeService` probes and
  indexes, `JobRunner` executes splits, `MergeRunner` executes merges, `WatchFolder`
  watches a directory, `FFmpegBootstrap` fetches and verifies the tools,
  `VerificationEngine` measures what was written. `FfmpegArgs` is the single place
  FFmpeg command lines are built, and it is pure.
- **`src/ui/`** - `UiController` owns every decision and contains no ImGui and no
  Windows headers, so it is unit-tested directly. `MainWindow.cpp` only draws what
  the controller exposes. A drawing bug can look wrong; it cannot report a job as
  verified when it was not.
- **`src/monitor/`** - resource metrics and the hardware encoder probe. Every metric
  that cannot be measured is reported unavailable with a reason, never as zero.

### Two conventions that are easy to get wrong

**A narrow `std::string` path means UTF-8.** Convert at the boundary with
`core/Utf8Path.hpp` (`wideFromUtf8`, `utf8FromWide`, `utf8CommandLineArguments`) and
never by widening a byte at a time. `std::filesystem::path` holds UTF-16 natively.
This is not a style preference: on Windows the C runtime hands `main` an `argv`
already decoded into the ANSI code page, so characters that code page cannot represent
are gone before your code runs, and passing the survivors to `std::filesystem` raises
`filesystem_error("Illegal byte sequence")`.

**Every FFmpeg seek is on FFmpeg's timeline, not the plan's.** The plan works in
absolute presentation timestamps (what ffprobe reports); `-ss` and `-to` are measured
from the container's `start_time`. Use `JobRunner::seekTime`. They coincide only when
a file starts at zero, so the bug is invisible on ordinary files and destructive on
the rest.

### Threading

- `UiController::poll()` is called once per frame and never blocks; the worker pushes
  events under a mutex.
- Exactly one job of either kind runs at a time. A merge and a split writing into the
  same output folder would collide on names.
- Long real work is off the UI thread: the FFmpeg bootstrap, the hardware encoder
  probe, each job, and the watch folder all have their own thread.
- The watch folder's callback does the least possible - push the path and return.
  Probing or running a job there would stop the watcher noticing the next arrival.

### Other facts worth knowing

- The exact-copy path runs one FFmpeg pass per stream and then muxes, because
  `-frames` is a stop condition for the whole output and cannot limit two streams at
  once. The exact rules `-ss`/`-t`/`-frames` do and do not obey are recorded in
  `src/engine/FfmpegArgs.hpp` with the measurements behind them.
- MonoSect does not trust the FFmpeg it finds: `DependencyPin` verifies the archive
  and each executable against digests compiled in by CMake. Update `resources/ffmpeg-pin.json`, then independently verify the archive and both
  executables; CMake and both workflows read that same pin.
- FFmpeg availability is not usability. `HardwareCapabilityProbe` asks each listed
  encoder to encode a real frame, because a static build lists every vendor encoder
  whether or not the hardware exists. Only a listed **and** usable encoder may be
  offered, or a requested stream copy silently becomes an encode.
- The help topics in `src/ui/HelpContent.cpp` describe panels that must exist. If a
  topic names a control, there is a panel that draws it.

### Release validation

Use `tools/prepare-ffmpeg.ps1` and `tools/package-release.ps1`; do not duplicate pin
values in workflows. Package only after unit, harness-safety and real-media checks
pass. `Monosect.exe --smoke-test` renders one frame without starting jobs or
network bootstrap. The release workflow creates a draft for human review.
Do not infer GPU job acceleration from the hardware probe: jobs currently use
software encoders. The end-to-end harness only removes directories carrying its
ownership marker and redirects job history away from the real user profile.
