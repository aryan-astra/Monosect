<div align="center">

<img src="Monosect-full-logo.png" alt="Monosect Logo" width="620" />

# MonoSect

**Split any media file into pieces that never exceed the limits you set — Windows 10/11, C++23, offline.**

![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D6?style=flat-square&logo=windows&logoColor=white)
![Language](https://img.shields.io/badge/C%2B%2B-23-00599C?style=flat-square&logo=c%2B%2B&logoColor=white)
![Build](https://img.shields.io/badge/Build-CMake%20%2B%20MinGW--w64-064F8C?style=flat-square&logo=cmake&logoColor=white)
![Media](https://img.shields.io/badge/Media-FFmpeg%20(pinned)-007808?style=flat-square&logo=ffmpeg&logoColor=white)

</div>

---

## What MonoSect is

MonoSect takes one media file and writes it out as parts that respect **hard
limits** you choose:

* **a maximum size per part, in exact bytes** — `2 GiB` means `2,147,483,648
  bytes`, and no part is ever published one byte above it;
* **a maximum duration per part, in exact rational seconds** — the same "never
  exceeded, not even by one frame" rule;
* while getting as close to the limit as it can, and telling you honestly how
  close it got.

Every written part is verified before it is reported, and every job is recorded
in a local history you can revisit.

MonoSect is not a video editor. It does not filter, grade, transcode for fun or
upload anything. It cuts, it verifies, it remembers.

## What MonoSect guarantees, and what it does not

| Mode | What it does | What it does **not** promise |
|---|---|---|
| **Stream copy** | Every packet is copied unchanged; cuts land on existing keyframes. | That the cut is where you asked: it is at the nearest keyframe. |
| **Keyframe-aligned copy** (default) | Copied packets, boundary chosen as close to the request as the keyframes allow. | Exact boundaries. |
| **Smart cut** | The body of each part is copied; a short region around each boundary is re-encoded so the boundary can be exact. | Bit-identical output — the re-encoded regions are not. |
| **Full re-encode** | Everything is encoded again; used when the container or codec must change. | Any kind of losslessness. |

The words *lossless*, *exact*, *frame accurate* and *verified* are used in
MonoSect only where they are literally true. A part that a verification step
could not check is shown as **not verified**, never as "verified".

The size and duration limits are separate from all of this: they are hard, and a
part that breaks one is rewritten smaller or the job fails. Nothing is ever
published oversized, and a plan that cannot fit inside the limits is refused
with the number of parts it would need.

## Features

* **Splitting** by size, by duration, or at one exact position, with optional
  per-stream preservation.
* **Precision policies** — fast, balanced (calibrates from the first written
  part) and maximum (extra search for the closest safe part).
* **Post-write verification** — size, duration, stream signatures, first-packet
  keyframe, decoded frame hashes and content hashes; failures are reported, not
  hidden.
* **Transactional output** — parts are written into a working folder on the
  destination volume and renamed into place; a part is either absent or complete.
* **Real cancel and pause** — cancel finishes the part in flight and keeps the
  verified parts (the log says exactly how many); pause takes effect at the next
  part boundary, which is stated in the interface rather than implied.
* **Job history** — every job that reaches a terminal state is appended to a
  local NDJSON database with the plan, the parts, the versions and the outcome.
* **Merging** — joining parts back with copy semantics, refusing inputs whose
  streams do not match, and verifying the result per stream.
* **A help system** — twenty topics, each with a one-line summary, the full rule
  and a worked example; the same text is used for the tooltips.

## Building

Requirements: CMake ≥ 3.24, MinGW-w64 g++ with C++23 support, and a Windows 10/11
SDK for the GUI. The core, the command line harness and the unit tests build on
Linux and macOS too, which is how the engine is developed and tested.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The GUI target `Monosect` is skipped with an explanatory message on any platform
that is not Windows. There is no vcpkg and no network access at run time: the
third-party libraries are fetched at configure time, and the media tools come
from a pinned address whose SHA-256 is verified when the release defines one
(`-DMONOSECT_FFMPEG_SHA256=...`). Without a pinned hash — as in a developer
build — the hash of the first download is recorded next to the binaries and
enforced afterwards, so a tool that is swapped later is still detected.

## Verification

Two suites run in CI and on any developer machine:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

* `monosect-tests` — the rules that must never regress: exact byte arithmetic,
  rational time arithmetic, packet indexing, planning against a packet index,
  size and duration limits, naming, verification and the UI's wording.
* `end-to-end` (`tests/harness/end_to_end.sh`) — drives the real FFmpeg tools
  through `monosect-cli` and checks the written files with an **independent**
  Python checker (`tests/harness/check_parts.py`) that parses the parts rather
  than trusting MonoSect's own report.

Checks that need Windows (the GUI, the job-object process runner, the pinned
toolchain download, NVML/PDH telemetry, hardware encoders) are listed in
[`docs/RESEARCH.md`](docs/RESEARCH.md) and in the release notes.

## Repository layout

```
src/core/      platform-neutral rules: bytes, rational time, media model,
               packet index, constraints, planning, verification, job state
src/engine/    process runners (POSIX/Windows), ffprobe front end, argument
               construction, output commit, job runner, merge, tool bootstrap
src/ui/        UiController (tested state machine), UiModel (all wording),
               HelpContent, MainWindow (Windows drawing only)
src/monitor/   measured resource telemetry (n/a rather than zero when unknown)
tests/         unit tests, the CLI harness and the end-to-end suite
docs/          research basis and design notes
```

## Licence and credits

MonoSect drives FFmpeg rather than linking it; see
[`docs/RESEARCH.md`](docs/RESEARCH.md) for the pinned versions and the reasoning.
