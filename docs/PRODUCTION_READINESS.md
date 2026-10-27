# Release candidate readiness

This records the production hardening performed on 2026-10-27. The original
[audit](audits/PROJECT_AUDIT_AND_ROADMAP.md) remains a historical snapshot. The
implementation branch is `codex/production-hardening`. GitHub run and release
status must be checked separately from these local validation results.

## Completed

- Correct merge-name capacity and synchronize bootstrap progress.
- Deliver merge results to the UI thread instead of mutating its live state.
- Consume completed split results once, avoiding repeated per-frame result copies.
- Freeze completed elapsed time and prevent old results returning after source changes.
- Block conflicting split/merge actions, invalidate changed output folders, route
  shortcuts to the visible session, and discard stale source plans.
- Own native dialogs, preserve the working directory and clear stale dialog input.
- Distinguish preview-only results and wrap progress details below the progress bar.
- Preserve cancelled status during verification and stop publication after cancellation;
  exercise that boundary with deterministic harness injection.
- Reject size-read errors, requested decode failures and failed/mismatching hashes.
- Fail incomplete sequences while retaining already published parts; show warnings.
- Replan from the actual successful retry boundary and retain the final actual plan.
- Preserve audio before the first video frame; measure packet spans as well as
  container durations. Missing content is not excused by increasing tolerances.
- Support long internal Windows paths, including cleanup; use relative concat
  entries and a working directory for smart-cut intermediates.
- Refuse path traversal, reserved output names and overwriting source inputs.
- Stop publication if flushing the verified file fails.
- Use the existing exact size/time parsers in the GUI, including overflow rejection.
- Improve narrow-column labels and run controls, expose full decode verification,
  disable conflicting edits during jobs and invalidate edited merge plans.
- Remove GPU job switches that were not connected to execution; hardware detection
  remains informational. Re-encoding currently uses software encoders.
- Verify manually located tools against pins. First-run download/extraction honors
  shutdown cancellation and has network/extraction timeouts.
- Preserve Unicode Windows profile paths for history.
- Protect end-to-end work directories with an ownership marker and isolate history.
- Replace the nonexistent-drive test with an owned local failure fixture.
- Share FFmpeg 9.0.2 archive and executable pins across app, CI and release scripts.
- Pin existing GUI dependencies and CI actions to source revisions.
- Build and run real tests before packaging; produce draft releases for review.
- Package runtime DLLs, pin metadata, build manifest, license and upstream notices.
- Smoke-test the packaged GUI with compiler directories removed from PATH.
- Apply the owner's selected GPL-3.0-only license to MonoSect's own source.

## Observed validation

Environment: Windows, MinGW GCC 15.2.0, CMake 4.1.0, Ninja, Python 3.13.15 and the
digest-checked FFmpeg 9.0.2 essentials build. GUI dependencies were reused from
the existing local checkout after verifying their revision IDs matched the pins.

- Full Windows GUI/core/CLI build with strict core warnings: passed. Third-party
  code emits warnings; strict core warnings no longer break on third-party code.
- All 120 unit tests (1,069 checks) and the harness-safety CTest suite: passed, including long temporary paths,
  failed verification, retry boundaries, safe work directories and cancelled bootstrap.
- Full real-media run: 149 passed, zero failed, zero skipped. An independent
  frame-count check found all 1,800 source frames across the eight winning-retry
  outputs: 240, 238, 239, 236, 240, 237, 237 and 133 frames.
- The independent frame-count assertion is now part of the permanent harness.
- Wrong FFmpeg archive checksum: rejected before extraction or installation.
- Prepared pinned archive: matched the publisher's checksum; both executables were
  hashed and validated before execution.
- Portable package: launches and renders one frame with PATH containing only
  Windows system directories; required GCC/MinGW DLLs are included. Smoke testing
  checks the tool pins compiled into the GUI as well as the packaging JSON.
- Replacing the packaged FFmpeg with an invalid test file causes smoke testing to
  fail before executing the tool; the verified binary was restored afterwards.
- Desktop UI inspected at the available display scale. ImGui exposes the native
  window controls but not meaningful application controls in the observed UIA tree.
- Split and merge panels inspected after the layout fixes. PowerShell and Python
  syntax checks, workflow YAML parsing and Git whitespace checks passed.
- Actual first-run setup without bundled tools downloaded, verified and extracted
  FFmpeg successfully and reached the ready screen. Native file selection was
  retested after the dialog fix; source reading, keyboard preview and a complete
  verified split were exercised through the desktop UI.
- Public release packaging omits FFmpeg executables and uses that verified upstream
  download on first launch. Bundled-media local packages remain a separate option.
- Hosted GCC 16 strict compilation, unit checks and the 149-check media suite passed
  for revision `0ac2168`. A later cancellation regression adds three checks; the
  focused eight-check cancellation section passed locally with the corrected engine.
- The first hosted release failed at GUI startup despite passing build and media
  checks. Packaging now captures GLFW diagnostics and can use MSYS2 Mesa for the CI
  smoke test. That software driver and its extra libraries are removed before ZIP creation.

Key commands:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMONOSECT_STRICT_WARNINGS=ON
cmake --build build -j 4
ctest --test-dir build -E monosect-end-to-end --output-on-failure
./tools/prepare-ffmpeg.ps1 -Destination ffmpeg
$env:MONOSECT_FFMPEG = (Resolve-Path ffmpeg/ffmpeg.exe).Path
$env:MONOSECT_FFPROBE = (Resolve-Path ffmpeg/ffprobe.exe).Path
python -B tests/harness/end_to_end.py build/monosect-cli.exe e2e-work
./tools/package-release.ps1 -BuildDirectory build -ToolsDirectory ffmpeg -Destination release
```

## Before public distribution

The result is tested against the cases above, not certification of every supported
machine or media format. Remaining validation and limitations:

1. Inspect each release workflow result. Hosted CI is verified as recorded above;
   no release signing key is configured, so checksum files are unsigned.
2. Test clean supported Windows 10 and 11 machines, high-DPI/multiple-monitor
   layouts, shutdown during a real first-run download, and representative user media.
3. Resolve screen-reader accessibility. ImGui keyboard navigation is enabled but
   the observed application controls are not exposed through Windows UI Automation.
4. Public packages download FFmpeg directly from upstream instead of redistributing
   it. Before distributing a bundled-media package, review Corresponding Source and
   complete the obligations for that exact FFmpeg build and its linked libraries.
   Included notices and source links do not substitute for those obligations.
5. Configure a private vulnerability-reporting channel and review dependency
   advisories. No complete advisory scan, secret-history scan, fuzzing or sanitizer
   run has been performed.
6. Profile long recordings before changing memory behavior. Packet indexing and
   decoded verification remain potentially expensive; no throughput or memory
   improvement is claimed. Source/merge preview probing still runs synchronously.

No global runtime migration, cloud service, editor rewrite or speculative feature
expansion was introduced. Inspect `build-manifest.json` in each package for its
actual source revision, dirty state, compiler and FFmpeg version.
