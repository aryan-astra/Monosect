# MonoSect: project audit and roadmap

**Audit date:** 2026-10-27 (Asia/Calcutta)  
**Repository:** [aryan-astra/Monosect](https://github.com/aryan-astra/Monosect)  
**Audited checkout:** `main`, `d892e22d8a0fd94631dd5d401640ad594d1f159b`  
**Remote at inspection:** `main` `af73abdc7841ab1742be1474d289f17a18c2abea`; local main is 21 commits ahead.  
**Contract:** inspection and this report only; no application, dependency, Git, or remote changes.

Confidence labels: **confirmed** means the stated code path or observed result is directly established; it does not imply that every failure was reproduced. **Likely** means the effect follows from the code but has not been demonstrated end-to-end. **Possible** identifies a hypothesis. **Unverified** means evidence is missing. Priorities describe actual impact, not confidence. Source links are repository-relative and refer to the audited checkout.

## 1. Executive summary

MonoSect is a Windows desktop application for splitting media under byte and duration limits, with merge, verification and local history. Its core uses explicit rational arithmetic, planning, process isolation and per-part checks; the independent FFmpeg-based checker is a valuable second opinion. The local 2.0.0 code is a substantial recent engine replacement and should be treated as a promising beta, not a proven production release. The published 1.1.0 is a different, older codebase and these local findings must not automatically be attributed to it.

The most urgent findings concern safe input handling and truthful results: a Windows merge-name buffer is passed the wrong capacity; sequence errors do not stop a success state; the closeness retry changes a part without updating later boundaries; failed size reads can be counted as zero; and optional decode/hash failures become informational notes. The release job also does not actually run the current test suites. The pinned upstream FFmpeg release currently returns HTTP 404, blocking the configured download path. Prioritize these before adding product surface.

## 2. Project overview

The product is a C++23 Windows/MinGW GUI using GLFW, Dear ImGui, OpenGL and external FFmpeg/ffprobe. `src/main.cpp` starts the GUI. `src/ui/MainWindow.cpp` renders controls and bootstraps tools; `UiController` manages UI state and jobs; `src/core` models inputs, constraints, packet indices, plans and verification; `src/engine` probes, builds process arguments, executes parts, verifies and commits files. `src/monitor` supplies resource and encoder information. `tests/harness/monosect_cli.cpp` is a test harness, not a documented end-user CLI.

A split probes input and packets, validates a constraint and output names, plans boundaries, renders to a job directory on the destination volume, measures and verifies each part, then publishes it. Merge checks compatibility, constructs a concat manifest, writes a temporary output and verifies before publishing. Job records are NDJSON. Platform-specific process runners separate Windows and POSIX; the latter supports portable core development, not the GUI.

## 3. Environment and audit coverage

Windows, PowerShell, CMake 4.1.0, Ninja, MinGW GCC 15.2.0, Python 3.13.15, Git 2.56.0 and installed FFmpeg/ffprobe were available. `gh` was not installed. Network access worked for Git and public GitHub pages; unauthenticated GitHub API requests were rate-limited. Public pages were used for repository, workflow and competitor information. GitHub and documentation MCP capabilities were discoverable but not used; no credentials, plugins or dependencies were installed. The available `code-review` skill was read but is a fixed-diff, two-agent review and did not match this project-wide audit, so its workflow was not applied.

Inspected repository inventory, README, CMake, workflows, history, representative planning, verification, publishing, merge, bootstrap, UI/controller, process and tests. This is risk-focused rather than a line-by-line proof of all files. The project had no tracked or untracked changes before the report; ignored local notes, patch and build tree were preserved. Commands included `git status --short --branch`, `git log`, `git branch -avv`, `git tag`, `git ls-files`, `git ls-remote origin`, `rg`, `cmake -S . -B <temporary-build> -G Ninja -DMONOSECT_BUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Release`, and `cmake --build <temporary-build> -j 4`. Configure and the Windows core/CLI/test build succeeded. Tests were isolated by setting TEMP, TMP and LOCALAPPDATA to new temporary directories; `Z:` was checked absent because one test uses a hard-coded nonexistent drive. Test results are recorded in the validation appendix below.

GUI rendering, screen readers, real hardware, full release packaging, signed artifacts, clean-machine dependencies, production data, remote branch protections, private advisories and authenticated Actions logs were not verified. No vulnerability scanner, complete secret-history scan, performance benchmark, sanitizer or coverage run was performed. Existing ignored `build-integration` was not treated as evidence of this checkout's build. A GUI configure generates a tracked resource icon in the source tree, so the isolated configure deliberately disabled the GUI.

## 4. Git and GitHub health

The public default branch is `main`. `git ls-remote` confirmed `af73abd` for remote `main` and `6dac6e2` for `canary`; local `main` was clean and 21 commits ahead. `integration-verified` is a local tag at `c0bf6b1`, not present in remote refs. Public [release v1.1.0](https://github.com/aryan-astra/Monosect/releases/tag/v1.1.0) points to `af73abd`; the local `version.txt` says 2.0.0. The public [CI run](https://github.com/aryan-astra/Monosect/actions/runs/25287008715) and [release run](https://github.com/aryan-astra/Monosect/actions/runs/25312250369) succeeded for the older commit, not the audited local code. The run pages show a historical Node 20 action warning; present compatibility needs verification against current action releases. The public issues page showed zero open and the PR page zero open and zero closed; API rate limiting prevented a full authenticated review of closed issues, advisories and settings.

The 2026-10-04 engine replacement and many follow-up correctness fixes demonstrate active integration but also concentration of unvalidated local work. `MainWindow.cpp`, CMake and the end-to-end harness are frequently changed. There is no tracked LICENSE, contribution guide, security policy or changelog; absence of an explicit license materially limits external reuse. Git history and current files were not exhaustively scanned for secrets. Git ignore excludes local scratch, patch, builds and notes; their presence is not evidence they are disposable.

## 5. Architecture assessment

**Strengths:** explicit plans and exact limit representations, platform-specific process adapters, atomic/no-overwrite publication intent, independently checked synthetic media, tested presentation model, and checked dependency digests are well suited to the product. Preserve these boundaries and avoid a rewrite.

**Weaknesses:** the orchestration in `JobRunner.cpp` has multiple mutable representations of a plan, measurements and published outcomes; a successful retry does not update the shared plan. Sequence status is not propagated to completion. MainWindow combines rendering, bootstrap and integration in one large translation unit. These are concrete coordination and testability risks, not reasons to add layers wholesale.

## 6. Code quality findings

- **AUDIT-001, P1, confirmed capacity mismatch / likely out-of-bounds write:** [`MainWindow.cpp:1635`](../../src/ui/MainWindow.cpp#L1635) passes the 32,768-byte output-folder buffer size for `mergeNameBuffer_`, which is 256 bytes ([declarations](../../src/ui/MainWindow.cpp#L2353)). Long pasted or entered merge names can cause ImGui to write beyond the actual array. Fix the size and test long input on the real GUI. Effort low; risk low except input-length behavior.
- **AUDIT-002, P1, confirmed status logic:** [`JobRunner.cpp:1183`](../../src/engine/JobRunner.cpp#L1183) collects sequence errors as warnings and then unconditionally sets success at line 1201, despite [`Verification.cpp:269`](../../src/core/Verification.cpp#L269) classifying missing coverage as an error. The UI derives `workerVerified_` from part flags only ([`UiController.cpp:535`](../../src/ui/UiController.cpp#L535)). Keep already published parts, but expose a failed/incomplete sequence through outcome, history and UI. Reproduce with injected sequence findings; actual media trigger not reproduced.
- **AUDIT-003, P1, likely overlap:** [`JobRunner.cpp:1020`](../../src/engine/JobRunner.cpp#L1020) accepts a larger retry and replaces only local `segment` at line 1043; the next iteration reads `plan.segments[index]` at line 903. No successful-retry path updates/replans remaining segments. A later part can start at the old boundary, repeating content. Verify actual boundaries with a fixture that forces a winning retry; update the plan and sequence checks together. Moderate effort and high regression risk around final segments and names.
- **AUDIT-004, P1, confirmed fail-open branch:** [`VerificationEngine.cpp:53`](../../src/engine/VerificationEngine.cpp#L53) substitutes zero on `file_size` error; [`Verification.cpp:73`](../../src/core/Verification.cpp#L73) treats a value below the limit as valid. Return a measurement error and do not publish when size cannot be read. Exercise the failure with an injected filesystem seam; a real filesystem failure was not reproduced.
- **AUDIT-005, P1, confirmed verification semantics:** [`VerificationEngine.cpp:77`](../../src/engine/VerificationEngine.cpp#L77) records nonzero decode status as a note, and lines 100–143 similarly treat failed or mismatching optional hashes as notes. [`Verification.cpp:157`](../../src/core/Verification.cpp#L157) maps notes to informational findings. A requested check can fail while the part says verified. Make requested checks explicit pass/fail/unavailable, with a deliberate policy for each; validate actual corrupt media and simulated subprocess errors.
- **AUDIT-006, P1, confirmed unsynchronized access:** [`MainWindow.cpp:521`](../../src/ui/MainWindow.cpp#L521) and line 554 write `bootstrapRunning_` on different threads; UI reads it at 1022 and 1049, and the declaration at 2337 is a plain `bool`. This is a C++ data race, with undefined behavior; visible failures are not reproduced. Use a consistent atomic or UI-thread message and test both success and failure/shutdown.
- **AUDIT-007, P2, confirmed ignored durability failure:** [`JobRunner.cpp:1079`](../../src/engine/JobRunner.cpp#L1079) logs a failed flush and publishes anyway. Durability after power loss cannot be guaranteed; decide and document whether flush failure must fail the job. Preserve verified temporary data where practical. Do not imply this demonstrates ordinary corruption.
- **AUDIT-008, P2, confirmed manual tool verification gap:** the manual selection path in [`MainWindow.cpp:1055`](../../src/ui/MainWindow.cpp#L1055) sets arbitrary executable paths and `toolsReady_` without calling the bootstrap digest checks; nearby UI claims the tools are SHA-256 identified before running. Validate/explain explicitly selected binaries and clearly distinguish trusted pinned binaries from user-selected tools. This is not evidence of a remote exploit.
- **AUDIT-009, P2, confirmed report drift and test hazards:** `README.md` says no runtime network, yet bootstrap downloads missing tools; `.github/copilot-instructions.md` says no POSIX build despite CMake's POSIX branch. `end_to_end.py:763` recursively deletes its supplied work path and `JobStoreTests.cpp:96` assumes `Z:` is absent. Document first-run behavior and isolate/guard test paths before CI or contributor use.
- **AUDIT-010, P1, release gate:** [release workflow:57](../../.github/workflows/release.yml#L57) builds only the GUI and lines 59–64 run a conditional `MonosectQualityChecks` executable absent from current targets; therefore the step can succeed without tests. Make unit and pinned end-to-end checks mandatory before publishing. Ensure clean-machine smoke test and source/asset traceability; do not infer present releases were tested by this local workflow.
- **AUDIT-014, P1, observed unavailable pin:** HTTP HEAD of the exact configured [FFmpeg asset](https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-03-18-14/ffmpeg-N-127142-g12b7b9891b-win64-gpl.zip) returned 404 on 2026-10-27; the upstream [release tag](https://github.com/BtbN/FFmpeg-Builds/releases/tag/autobuild-2026-10-03-18-14) also returned 404 in a separate browser fetch. This blocks the configured fresh bootstrap and CI/release download when used; a system with usable local tools may have a separate path. Refresh the immutable artifact and independently verify archive and executable digests together across CMake and both workflows, or provide a maintained verified mirror. Availability may change; no large download was attempted.
- **AUDIT-015, P1, observed hang, cause unconfirmed:** the isolated Windows end-to-end long-filename case ran for minutes with high CLI CPU and no FFmpeg child. A non-destructive `gdb` attachment showed the main thread inside `OutputCommitter::cleanUp()` / `std::filesystem::remove_all` ([`OutputCommitter.cpp:170`](../../src/engine/OutputCommitter.cpp#L170)). A retained temporary file path was 279 characters. The test CLI was terminated so the remaining suite could proceed. This is one observed environment, not proof that path length alone causes the hang; reproduce on supported clean Windows/toolchains and establish a bounded, safe cleanup path. Do not delete the evidence until it is no longer needed.

`MainWindow.cpp` is ~2,255 nonblank lines and `JobRunner.cpp` ~1,154: large files increase review cost, but size alone is not a defect. Split only cohesive seams when fixing or testing them. Confirmed uncalled `OutputCommitter::removeStaleJobDirectories` appears only in its declaration and definition; preserve it until API/maintenance intent is checked. No dependency was proven unused. Ignored local patches and notes are user data, not cleanup candidates.

## 7. Bugs and correctness risks

Confirmed code defects: AUDIT-001, 002, 004–006. AUDIT-003 is a strongly supported execution-path risk until a won-retry media case is reproduced. The sequence behavior misstates an outcome if the verifier reports an error; it does not establish that all jobs lose media. A `file_size` error is a rare error path, not proof an oversized output has already shipped. Optional content-hash comparison also excludes the final segment in its current condition; whether that is acceptable must be explicit.

Other risks for targeted investigation: a failure to append history is deliberately best effort and recorded only as a warning for successful outcomes; failure-record appends are ignored ([`JobRunner.cpp:751`](../../src/engine/JobRunner.cpp#L751), [`MergeRunner.cpp:643`](../../src/engine/MergeRunner.cpp#L643)). Test collisions and source/output aliases with `overwriteExisting` through the CLI before claiming a data-loss bug; GUI merge currently sets overwrite false. Race handling in the Windows no-overwrite move is useful, although the message promises a rewrite while the runner returns failure if a late collision occurs.

## 8. Security assessment

The relevant trust boundaries are user-selected files, media parsers, filesystem paths, child executables, archive download/extraction and releases. AUDIT-001 is a local memory-safety defect; exploitability beyond long local input is unverified. AUDIT-008 allows explicitly selected programs to run without pinned verification; the user's choice is authorization to select a tool but the trust wording should be accurate. The normal bundled/download path checks archive and executable SHA-256 pins; developer trust-on-first-use detects later changes but cannot authenticate the initial file. Windows process execution uses `CreateProcessW` with arguments and a job object rather than a shell for normal FFmpeg execution. PowerShell is used for archive extraction; review paths and quoting when changing bootstrap.

Do not claim a vulnerability from dependency versions without advisory matching. No current CVE audit, full history secret scan, third-party penetration test or malicious media fuzzing was performed. Explicit source/output alias protection, path canonicalization, archive handling and release executable dependencies deserve focused verification. No credentials should be copied into reports.

## 9. Testing and reliability

Unit tests cover arithmetic, parsing, packet indices, planners, verification, naming, job history, UI models/controllers, dependency pins and watch readiness. The isolated unit suite passed. `tests/harness/end_to_end.py` generates synthetic files, drives actual tools and uses `check_parts.py` independently; it tests several formats, names, merge, collision, closeness, history and cancellation. The run reported 129 passed, one failed and zero skipped; the long-path failure followed manual termination of the stalled CLI, not an independently completed test verdict. However, unit-testing `evaluateSequence` does not exercise the runner's response to its error; a passing closeness test checks existence rather than full original coverage and non-overlap. Real GUI, first-run bootstrap, thread safety, exception/failure injection, signed release and real-machine accessibility are gaps.

Highest-value tests: long merge names with a memory checker; forced sequence error through runner/controller/history; successful retry with independent source-to-parts timeline/packet coverage; failed stat/flush; failed optional verification; pinned-tool selection; bootstrap success/failure/shutdown; clean Windows release extraction and execution. Tests should use unique temporary roots and redirect local history. Avoid a blanket coverage target as a substitute for these checks.

## 10. Performance assessment

No benchmarks or profiling were performed. `MergeRunner.cpp:173` captures full frame-hash text and `:558` builds vectors for merged and input video; long, high-frame-rate media can consume memory proportional to frame count and require full decoding. Packet probing also creates substantial process output and indexing work. Treat these as hypotheses: measure peak memory, latency and I/O on representative long and multi-stream files before streaming hashes or changing indexing. Preserve correctness comparisons when optimizing. `JobStore::loadRecent` reads and reverses the entire history before limiting ([`JobStore.cpp:103`](../../src/engine/JobStore.cpp#L103)); likely growth concern, not an established user bottleneck.

## 11. Dependencies and tooling

CMake pins GLFW 3.4, ImGui v1.91.5 and FFmpeg archive/executable hashes, but CI installs the current MSYS2 toolchain and actions by mutable version tags. FFmpeg pin values are repeated in CMake, CI and release workflow; a coordinated refresh is error-prone. Consider one verified source of pin metadata without replacing working controls. The configured upstream asset and release tag returned 404 during this audit (AUDIT-014); availability can change. Establish a pin-refresh process. No advisory or compatibility determination was made.

The GUI CMake configure rewrites `resources/Monosect.ico` inside the source tree ([`CMakeLists.txt:332`](../../CMakeLists.txt#L332)); move generation into the build tree in a later build-hygiene task and verify Windows resource paths. Current release packaging may rely on MinGW runtime DLLs; actual clean-machine startup and license/redistribution obligations are unverified and must be checked before release. The optional signature covers the standalone EXE, not the ZIP or checksum list; do not claim the bundle is authenticated without verifying its signing chain.

## 12. UI/UX and accessibility

Source shows keyboard navigation enabled, shortcuts, DPI handling, warnings and explanatory help. No GUI was launched or exercised in this audit; rendering, focus order, contrast, scaling, screen-reader exposure and native dialogs remain unverified. ImGui keyboard support alone does not establish accessibility. Test long input, a 100%/high-DPI layout, keyboard-only split/merge/cancel, failure details and screen-reader compatibility on Windows. The status wording should distinguish individually verified parts, sequence completeness, skipped checks and history write failures.

## 13. Repository cleanup plan

1. Correct README and Copilot instructions only after testing the documented path; add first-run network and tool trust language. Low risk.
2. Add a license only after the owner selects one and reviews bundled FFmpeg obligations; do not invent a license. Contribution/security files are useful only if the project intends external participation.
3. Move icon generation out of the source tree and correct isolated test paths. Verify resource compilation and avoid deleting tracked assets.
4. Retain the ignored local patch, research and audit notes and `build-integration`; owner intent and contents must be checked before any removal. Check `removeStaleJobDirectories` call sites outside the repository/public API before removing it. No bulk deletions are justified.

## 14. Feature inventory

| Capability | Assessment | Evidence / limitation |
|---|---|---|
| Size/duration and one-position split, planning | Implemented; behavioral verification limited to test results below | core constraints/planner, `JobRunner` |
| Copy, smart cut, re-encode, calibration/closeness | Implemented; retry continuity needs repair | `JobRunner.cpp:1007`; harness modes and closeness sections |
| Merge and per-stream checks | Implemented; GUI path not exercised | `MergeRunner`, `UiController`, harness |
| Boundary pause/cancel, history | Implemented; GUI not observed | controller and runner; history tests |
| Watch folder and hardware probe | Implemented in Windows wiring; real hardware/watching not tested | `MainWindow.cpp`, `WatchFolder.cpp` |
| Decode/hash verification | Implemented but failure semantics incomplete | AUDIT-005 |
| Full sequence integrity outcome | Partial: computed, not enforced | AUDIT-002/003 |
| Cross-platform desktop app | Not implemented; portable core/CLI only | CMake GUI guard |
| Reliable public 2.0 release | Unverified/not yet published at inspected refs | local vs remote SHA |

## 15. Feature suggestions

| Proposal and user | Behavior and relevance | Prerequisites / cost / risk | Value and decision |
|---|---|---|---|
| **Honest completion and recovery**, everyday splitter | Show each check's result, any sequence gap, published paths and the next safe action; protects the core promise. | AUDIT-002–005; medium effort; avoid misclassifying valid container rounding. | High; build now as reliability work. |
| **Output manifest**, archive/power user | Export a small machine-readable list of parts, measured sizes/durations, source identity and actual checks; aids audit and recovery. | Stable outcomes/history; medium; privacy of paths and stale source identity. | High; investigate after stabilization. |
| **Batch queue**, repeat user | Process multiple chosen sources with per-item result and existing per-job limits. | Reliable cancellation/history and UX study; medium/high; resource contention. | Potentially high; later, validate demand. |
| **Saved limit presets**, recurring upload user | Save named size/duration combinations, not arbitrary encoding profiles. | Existing settings and validated constraints; low/medium; stale settings migration. | Medium; later. |
| **Accessible operation**, keyboard and assistive-tech users | Test and repair focus, semantics, scaling and status announcements. | Windows UI audit; medium/high; ImGui platform limitations. | High; investigate early, implement verified fixes. |
| **Large-file diagnostics**, power user | Report time spent probing, encoding and verifying; add peak-memory measurements only when needed. | Reliable phase instrumentation; medium; avoid leaking paths in shared logs. | Medium; investigate before optimization. |
| **Remote upload / AI / full timeline editor**, all users | Outside the offline hard-limit split/verify job. | New trust, operational and maintenance surfaces. | Reject for now; no evidence of demand. |

## 16. Competitor and ecosystem comparison

[LosslessCut](https://github.com/mifi/lossless-cut) and its [documentation](https://losslesscut.app/docs) describe visual segments, track selection and project import/export. [Shutter Encoder](https://www.shutterencoder.com/documentation/) documents watch folders, merge, trimming and broad conversion/analysis. Those projects are broader; MonoSect's promising differentiator is explicit hard limits and transparent verification, but no independent comparative measurement proves it ahead. Competitor claims reflect their documentation, not hands-on verification. Do not chase feature parity or infer user demand from feature lists.

## 17. Technical debt register

The most consequential debt is not the code's size but the split between intended verification contracts and actual terminal outcomes (AUDIT-002–005), followed by unsafe UI input/state (001/006) and release validation (010). Secondary debt: duplicated dependency pins, path and temporary-directory assumptions, incomplete release dependency/license evidence, optional durability handling and stale contributor documentation. Maintain the exact arithmetic, output commit boundaries and independent checker while correcting orchestration.

## 18. Prioritized recommendations

| ID | Finding / opportunity | Category | Priority | Impact | Effort | Confidence | Evidence |
|---|---|---|---|---|---|---|---|
| AUDIT-001 | Correct merge-name buffer capacity | Security/correctness | P1 | High | Low | Confirmed code | `MainWindow.cpp:1635,2353–2362` |
| AUDIT-002 | Propagate sequence failures to status/history/UI | Correctness | P1 | High | Medium | Confirmed code | `JobRunner.cpp:1183–1203`, `Verification.cpp:269` |
| AUDIT-003 | Replan after winning closeness retry | Correctness | P1 | High | Medium | Likely impact | `JobRunner.cpp:903,1020–1043` |
| AUDIT-004 | Fail closed when size cannot be measured | Correctness | P1 | High | Low/medium | Confirmed code | `VerificationEngine.cpp:53–55` |
| AUDIT-005 | Make requested checks report actual failure/unavailable | Reliability | P1 | High | Medium | Confirmed code | `VerificationEngine.cpp:77–143`, `Verification.cpp:157` |
| AUDIT-006 | Synchronize bootstrap state | Concurrency | P1 | Medium/high | Low | Confirmed code | `MainWindow.cpp:521,554,1022,2337` |
| AUDIT-010 | Gate releases on tests and clean-package smoke | CI/release | P1 | High | Medium | Confirmed workflow gap | `.github/workflows/release.yml:57–64` |
| AUDIT-014 | Restore the unavailable pinned FFmpeg artifact | Dependency/release | P1 | High | Medium | Observed HTTP 404 | `CMakeLists.txt:131`; `.github/workflows/ci.yml:58`; `release.yml:88` |
| AUDIT-015 | Investigate long-path cleanup hang | Reliability | P1 | High | Medium | Observed once; cause unverified | `OutputCommitter.cpp:170`; isolated end-to-end long-path case |
| AUDIT-007 | Define and enforce flush failure policy | Reliability | P2 | Medium | Low/medium | Confirmed code | `JobRunner.cpp:1079–1083` |
| AUDIT-008 | Authenticate or disclose manual tool selection | Security/UX | P2 | Medium | Medium | Confirmed branch | `MainWindow.cpp:1055–1075` |
| AUDIT-009 | Fix documentation and destructive test-path assumptions | Docs/testing | P2 | Medium | Low/medium | Confirmed code | README; `end_to_end.py:763`; `JobStoreTests.cpp:96` |
| AUDIT-011 | Confirm redistribution, license and portable dependencies | Release/legal | P2 | High before distribution | Medium | Unverified risk | release workflow, missing LICENSE |
| AUDIT-012 | Measure large-file memory and verification time | Performance | P2 | Unknown | Medium | Hypothesis | `MergeRunner.cpp:173,558–576` |
| AUDIT-013 | Test accessibility and repair observed barriers | Accessibility | P2 | Potentially high | Medium/high | Unverified | `MainWindow.cpp:402` |

## 19. Recommended development roadmap

**A. Stabilization:** AUDIT-001–006 and AUDIT-015, with focused tests. Independently deliver the buffer and bootstrap fixes, but coordinate sequence, closeness and verification semantics so outcomes cannot conflict. Acceptance: no unsafe buffer access, failed checks cannot produce an unqualified success, retry coverage is independently validated, prior verified outputs survive failure, and cleanup terminates safely on the reproduced long-path case.

**B. Release foundation:** AUDIT-014, 010 and 011 plus safe isolated test setup. Restore a verifiable tool pin, then require Windows unit/end-to-end tests with those exact tools, clean-package launch and artifact checks tied to the same revision. No release is implied by this recommendation. Investigate licensing and distribution separately with the owner.

**C. Cleanup:** AUDIT-007–009, source-tree icon generation and selective readability improvements, without deleting ignored files. Acceptance: documentation matches observed behavior; tests cannot remove arbitrary supplied directories; no changes to functional contracts without regression tests.

**D. Product:** accessibility testing, honest diagnostics and a possible output manifest; validate user needs before queue/preset work. Acceptance: observed workflows and status messages verified on Windows.

**E/F. Advanced and maintenance:** profile representative long files, improve only measured bottlenecks, set pin-refresh and release ownership, and review dependency advisories periodically. Do not introduce an architectural rewrite.

## 20. Quick wins

Correct the buffer size; synchronize the one shared progress flag; change the stat error into a verification failure; correct contradictory documentation; guard test work paths. Each is small but needs the focused validation listed in the backlog.

## 21. High-risk changes to avoid

Do not publish 2.0 as production-ready on the strength of the old remote CI, delete or overwrite user media while repairing output behavior, remove ignored files, change frameworks, replace rational arithmetic, introduce background parallelism, or broaden into an editor/online service before the core completion promise is trustworthy. Do not weaken verification merely to make existing fixtures pass.

## 22. Implementation-ready backlog

Each task is independent unless dependencies are stated; all retain published files on failure and require tests using isolated directories.

**AUDIT-001 — Bound merge-name input.** Category security; P1; evidence `MainWindow.cpp:1635,2353–2362`. Pass the capacity of the actual name buffer, audit other `InputText` capacities and preserve valid names. Scope `MainWindow.cpp` and an appropriate GUI-focused test. Dependencies none. Acceptance: empty, boundary-length, oversized and Unicode input cannot corrupt adjacent state; valid names still work. Validate with Windows GUI plus memory instrumentation if available. Risk changed truncation; effort low; confidence high.

**AUDIT-002 — Honor sequence verification outcome.** Category bug; P1; evidence `JobRunner.cpp:1183–1203`, `UiController.cpp:535`, `Verification.cpp:269`. Propagate error or unavailable sequence status to terminal state, UI and history; retain and name already published parts. Scope runner/outcome/controller/history and tests. Dependencies coordinate with 003/005. Acceptance: a forced missing-coverage finding never reports complete success, warnings are visible, and good sequences still complete. Validate injected runner/controller/history regression and independent media fixture. Risk false failure from timestamp tolerance; effort medium; confidence high.

**AUDIT-003 — Keep retry and remaining plan continuous.** Category bug; P1; evidence `JobRunner.cpp:903,1020–1043`; impact likely. Update actual chosen boundary and replan remaining segments after a larger retry, or otherwise enforce contiguous measured coverage. Scope runner/planner/harness. Depends on 002 for final outcome. Acceptance: a proven winning retry yields ordered non-overlapping source coverage through last part while preserving hard limits. Validate independent packet/frame/timestamp checks across copy and re-encode, multi-part and final-part cases. Risk container timestamp and output-name changes; effort medium; confidence high in stale plan, medium in runtime consequence.

**AUDIT-004 — Fail verification on unreadable size.** Category bug; P1; evidence `VerificationEngine.cpp:53–55`; enforce failure on filesystem error. Scope verifier and focused tests; no dependencies. Acceptance: failed stat cannot be evaluated as zero or published, ordinary zero-size/readable behavior remains explicit. Validate injected error and existing size-boundary tests. Risk code paths previously hidden now fail; effort low/medium; confidence high.

**AUDIT-005 — Make optional checks truthful.** Category reliability; P1; evidence `VerificationEngine.cpp:77–143`, `Verification.cpp:157`. Represent failed, skipped and unavailable decode/hash checks and propagate the selected policy to results. Scope verifier, result model, runner, UI/history, tests. Coordinate with 002. Acceptance: enabled check failure cannot be reported as passed; unavailable checks are explicitly identified; disabled checks do not masquerade as performed. Validate corrupt fixture and subprocess failure, mismatch and last-segment cases. Risk codec/container hash equivalence; effort medium; confidence high.

**AUDIT-006 — Synchronize bootstrap progress.** Category concurrency; P1; evidence `MainWindow.cpp:521,554,1022,2337`. Use one atomic or UI-thread ownership for the flag and review related lifecycle. Scope GUI bootstrap; no dependencies. Acceptance: no unsynchronized access on success, failure, manual selection and shutdown; UI state remains responsive. Validate repeated Windows startup/shutdown and thread sanitizer where supported. Risk changed ordering; effort low; confidence high.

**AUDIT-007 — Make flush handling explicit.** Category reliability; P2; evidence `JobRunner.cpp:1079–1083`. Define policy and either fail safely on flush error or accurately expose degraded durability. Scope committer/runner and tests; no dependency. Acceptance: simulated flush failure produces documented result without destroying verified temporary bytes unintentionally. Risk filesystem compatibility; effort low/medium; confidence high in ignored error, medium in impact.

**AUDIT-008 — Clarify manual tool trust.** Category security/UX; P2; evidence `MainWindow.cpp:1055–1075`, `FFmpegBootstrap.cpp:52`. Validate both selected tools and identify whether they match pins; communicate explicit user override if supported. Scope bootstrap/UI/docs/tests. No dependency. Acceptance: pinned, mismatched, missing and user-selected binaries have accurate status before execution. Risk excluding legitimate custom binaries; effort medium; confidence high.

**AUDIT-009 — Make audit and contributor runs safe and reproducible.** Category testing/docs; P2; evidence `end_to_end.py:763`, `JobStoreTests.cpp:96`, README and `.github/copilot-instructions.md`. Use owned unique temp roots, remove the fixed Z-drive assumption, guard destructive work-directory handling and correct network/POSIX documentation. Acceptance: tests do not delete pre-existing arbitrary directories or touch real job history; instructions match actual commands. Validate on Windows and a portable platform. Risk CI workflow changes; effort medium; confidence high.

**AUDIT-010 — Require validated release inputs.** Category infrastructure; P1; evidence release workflow lines 57–64 and CI pinned FFmpeg block. Build and run tests rather than optional nonexistent quality binary; tie tested revision, verified tool hashes and packaged artifact together; test clean Windows extraction and launch. Depends on isolated tests (009). Acceptance: any failed/missing suite stops release, package starts on clean supported Windows and declared hashes match. Validate a nonpublishing workflow run and intentionally failing check. Risk runtime, upstream download availability; effort medium; confidence high.

**AUDIT-011 — Establish distribution rights and package completeness.** Category release; P2; evidence missing license, release workflow. Owner chooses license, reviews third-party notices and GPL obligations with appropriate expertise, verifies DLLs/signature/ZIP contents. Depends on 010 for package validation. Acceptance: distributable artifact has required runtime and notices, stated license and accurate signature claims. Risk legal and compatibility decisions; effort medium; confidence high in missing evidence, not a legal conclusion.

**AUDIT-012 — Measure verification costs.** Category performance; P2; evidence `MergeRunner.cpp:173,558`. Profile source duration, peak memory, disk and wall time on representative data, then propose streaming only if warranted. Depends on stable correctness. Acceptance: reproducible baseline, no unsupported speed claims, unchanged verification outcomes. Risk benchmark bias; effort medium; confidence hypothesis.

**AUDIT-013 — Verify accessible GUI operation.** Category accessibility; P2; evidence `MainWindow.cpp:402`. Observe keyboard, DPI, contrast, focus and assistive technology; fix confirmed barriers. Dependencies 001/006 advisable. Acceptance: documented Windows workflows operable with keyboard and observed screen-reader results, with limitations explicit. Risk ImGui integration scope; effort medium/high; confidence unverified.

**AUDIT-014 — Restore the pinned FFmpeg artifact.** Category dependency/release; P1; evidence the configured asset and upstream tag returned HTTP 404 on 2026-10-27. Select an immutable available build or maintained verified mirror; independently verify archive and executable digests and synchronize CMake, CI and release settings. No dependency; blocks pinned CI/release validation in 010. Acceptance: fresh bootstrap and CI retrieve the same authenticated artifact, reject a deliberately wrong digest and package the matching executables. Validate with a clean isolated Windows run and release candidate without publishing. Risk upstream retention, changed codec behavior and distribution obligations; effort medium; confidence high for the observed 404, availability may change.

**AUDIT-015 — Diagnose and bound long-path cleanup.** Category reliability; P1; evidence the end-to-end long-filename case stalled with high CPU in `OutputCommitter::cleanUp()` / `remove_all`, on a job containing a 279-character temporary-file path. Scope output committer, Windows path handling and harness. No dependencies. Acceptance: reproduce or explain the platform-specific failure; cleanup completes or reports a bounded, actionable error, preserves unrelated files, and jobs cannot spin indefinitely. Validate with an isolated long-path regression on the existing and a clean supported Windows setup, and failure injection. Risk destructive cleanup or concealing an underlying failure; effort medium; confidence high in observed hang, low in exact cause.

## 23. Final engineering verdict

1. **Genuinely good:** precise constraints, plan/execute/verify boundaries, Windows process isolation, no-overwrite publication intent and an independent integration checker.
2. **Five weaknesses:** unsafe GUI buffer; misleading sequence/check status; retry/plan desynchronization; incomplete failure-path validation; a release workflow without enforced current tests.
3. **Highest-risk problem:** users can be told a split is complete despite a sequence-level error, undermining trust in media completeness; the separate buffer defect is the most direct memory-safety concern.
4. **Largest unnecessary complexity:** overlapping mutable representations of plan, retry, measured segments and final outcomes inside the split runner; simplify only alongside behavior-preserving tests.
5. **Preserve:** rational/byte rules, platform isolation, independent checker, staged publication and explicit error models.
6. **Urgent areas:** `MainWindow` input/bootstrap, `JobRunner` sequence/retry, `VerificationEngine`, and release gates.
7. **Finish first:** truthful end-to-end verification and complete, non-overlapping output before additional product features.
8. **Highest-value feature:** clear verification/completion and recovery information built on corrected outcomes.
9. **Avoid:** framework rewrite, generic editor features, online/AI features and unmeasured optimization.
10. **First milestone:** AUDIT-001–006, 014 and 015 with focused regression tests, then AUDIT-009/010 to gate a future release. Details and acceptance criteria are in sections 19 and 22.

## Appendix: validation outcomes

- Isolated CMake configure and build: **passed**, using Windows MinGW GCC 15.2.0 with `MONOSECT_BUILD_GUI=OFF` and `-j 4`. This built the core, Windows process runner, CLI and tests, not the GUI.
- `ctest --test-dir <temporary-build> -R '^monosect-unit-tests$' --output-on-failure`: **passed**, 1/1 registered unit test executable in 4.10 seconds. TEMP, TMP and LOCALAPPDATA were redirected to a newly created isolated directory; `Z:` was confirmed absent before execution.
- `python tests/harness/end_to_end.py <temporary-build>/monosect-cli.exe <new-temporary-work>`: **exit 1; 129 passed, 1 failed, 0 skipped**. The long-filename case stalled for minutes with high CLI CPU; `gdb -q -batch -p <test-cli-pid> -ex 'thread apply all bt' -ex 'detach'` placed its main thread in `OutputCommitter::cleanUp()` / `std::filesystem::remove_all`. The audit terminated that test CLI and the harness continued; the one failure is therefore an interrupted hung test, not a normal completed verdict. Retained work and `e2e.log`: `C:/Users/Ryan/AppData/Local/Temp/monosect-e2e-31070dbf438f4903983c6312a425cb39/work/`. The suite used installed FFmpeg `2025-02-20-git-bc1a3bfd2c`, not the unavailable release-pinned binary.
- HTTP HEAD of the exact configured FFmpeg asset returned 404; the tag page independently returned 404. This is a point-in-time availability observation, not a hash verification.
- The public older commit's CI and release jobs succeeded as linked in section 4; no claim is made for local HEAD or artifact contents.
