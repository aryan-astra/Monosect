<div align="center">

<img src="Monosect-full-logo.png" alt="Monosect Logo" width="620" />

# Monosect
**Fast, lossless video splitting and merging for Windows**

![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?style=flat-square&logo=windows&logoColor=white)
![Language](https://img.shields.io/badge/C%2B%2B-23-00599C?style=flat-square&logo=c%2B%2B&logoColor=white)
![Build](https://img.shields.io/badge/Build-CMake%20%2B%20Ninja-064F8C?style=flat-square&logo=cmake&logoColor=white)
![Media Engine](https://img.shields.io/badge/Media-FFmpeg-007808?style=flat-square&logo=ffmpeg&logoColor=white)
![Release](https://img.shields.io/badge/Releases-GitHub-181717?style=flat-square&logo=github&logoColor=white)

</div>

---

## What Monosect does

Monosect splits large videos into smaller parts and merges compatible parts back into one file.

By default, it uses **strict lossless mode** (`-c copy`) so output quality stays identical to the source.

---

## Key features

| Area | What you get |
|---|---|
| Split modes | Exact time, exact size, half split, torrent presets, Telegram-safe preset, N equal parts |
| Quality | Strict lossless mode (no re-encode), keyframe-aware boundaries |
| Control | Per-file progress, queue progress, pause/resume, cancel |
| Safety | Minimum file-size threshold, optional post-split validation |
| Speed | Auto-tuner for worker/thread pressure balancing |
| Output | Custom output directory + filename pattern (`{stem}`, `{n}`, `{n:02d}`, `{ext}`) |
| Automation | Watch folder intake + GitHub release workflow for EXE assets |
| Merge | In-app lossless merge flow with reorder support |

---

## Downloading

Go to [Releases](../../releases/latest).

| File | Notes |
|---|---|
| `Monosect-vX.Y.Z-portable.zip` | Recommended. Includes `Monosect.exe`, `ffmpeg.exe`, `ffprobe.exe`. |
| `Monosect.exe` | Standalone app. Downloads FFmpeg on first run if missing. |

No installer. No admin rights required.

---

## Quick start

1. Download the latest release.
2. Run `Monosect.exe`.
3. Add files, choose split mode, click **Start Split**.
4. Use **Dry-run Estimate** before splitting if you want chunk preview.

---

## Keyboard shortcuts

- `Ctrl + O` Add files
- `Ctrl + Enter` Start split
- `Ctrl + P` Pause / Resume
- `Ctrl + D` Dry-run estimate
- `Delete` Remove selected queue entry
- `F1` Shortcut help

---

## Build from source

### Requirements

- Windows 10/11 (64-bit)
- MinGW-w64 (GCC 13+)
- CMake 3.24+
- Ninja

### Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target Monosect MonosectQualityChecks -j 8
ctest --test-dir build --output-on-failure --verbose
```

Run:

```powershell
.\build\Monosect.exe
```

---

## Versioning

- Canonical version file: `version.txt`
- Bump helper:

```powershell
.\tools\bump-version.ps1 patch
.\tools\bump-version.ps1 minor
.\tools\bump-version.ps1 major
```

---

## GitHub auto-build (EXE in Releases)

Yes — this repo is set up for it.

When you push a tag like `v1.1.0`, GitHub Actions will:

1. Validate the tag against `version.txt`
2. Build `Monosect.exe`
3. Create/update the GitHub Release
4. Upload release assets:
   - `Monosect.exe`
   - `Monosect-vX.Y.Z-portable.zip`
   - `SHA256SUMS.txt`
   - `Monosect.exe.asc` (only if signing secrets are configured)

### One-time setup you may need

1. Add a GitHub Actions runner permission that can write releases (`contents: write` is already set in workflow job).
2. (Optional, for signed binaries) set repository secrets:
   - `MONOSECT_RELEASE_SIGNING_KEY`
   - `MONOSECT_RELEASE_SIGNING_KEY_ID`

If you skip signing secrets, releases still build and publish normally.

---

## Verifying downloads

Check SHA256:

```powershell
Get-Content .\SHA256SUMS.txt
(Get-FileHash .\Monosect.exe -Algorithm SHA256).Hash.ToLowerInvariant()
```

Verify signature (if `Monosect.exe.asc` exists):

```powershell
gpg --verify .\Monosect.exe.asc .\Monosect.exe
```

---

## Project structure

```text
src/
  ui/         App UI, queue, controls, logs, metrics
  engine/     FFmpeg process, split logic, probe, threads, watch folder
  monitor/    CPU/GPU monitoring
  precision/  Size precision search helpers
resources/    App icon resources
tests/        Quality checks
tools/        Version helper scripts
```

---

## License

All rights reserved. Proprietary software by Monosect.
