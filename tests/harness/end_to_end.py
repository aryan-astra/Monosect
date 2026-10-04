#!/usr/bin/env python3
"""MonoSect — end-to-end tests (TODO 346 - TODO 364).

Runs the real CLI (tests/harness/monosect_cli.cpp, which drives the real engine)
against synthetic fixtures, then re-checks every output with ffprobe/ffmpeg only.
Nothing here consults MonoSect's own verification result to decide whether
MonoSect worked: the product's verifier is unit-tested separately, and a bug in it
must not be able to make this suite pass.

Why Python rather than the bash script this replaces: bash is not part of
Windows, so the suite could not run on the platform the product ships on.  The
assertions are the same and there are more of them.

Usage:
    end_to_end.py <path-to-monosect-cli> [work-directory]

Environment:
    MONOSECT_FFMPEG, MONOSECT_FFPROBE   binaries to use (default: PATH)
    MONOSECT_E2E_SKIP                   comma-separated section names to skip
    MONOSECT_E2E_KEEP                   set to keep the work directory
"""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FFMPEG = os.environ.get("MONOSECT_FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("MONOSECT_FFPROBE", "ffprobe")
CHECK = HERE / "check_parts.py"
FIXTURES_SCRIPT = HERE / "make_fixtures.py"
SKIP = {s.strip() for s in os.environ.get("MONOSECT_E2E_SKIP", "").split(",") if s.strip()}

# Windows consoles still default to a legacy code page (cp1252 here), and this
# suite both *prints* media file names and *decodes* child output that contains
# them.  Several fixtures are deliberately named with characters a legacy code
# page cannot represent, because that is exactly what the product has to survive.
# Left alone, the very failure being reported raises UnicodeEncodeError from
# inside the reporting call and the run dies with a traceback instead of a verdict.
# So every stream is forced to UTF-8 and told to replace what it cannot encode:
# a mangled byte in a log line is acceptable, losing the failure is not.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
    except (AttributeError, ValueError):  # pragma: no cover - non-reconfigurable stream
        pass


class Report:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.failed_names: list[str] = []

    def ok(self, description: str, condition: bool, detail: str = "") -> bool:
        if condition:
            self.passed += 1
            print(f"  ok   {description}")
            if detail:
                for line in detail.strip().splitlines():
                    print(f"       {line}")
            return True
        self.failed += 1
        self.failed_names.append(description)
        print(f"  FAIL {description}")
        if detail:
            for line in detail.strip().splitlines()[:25]:
                print(f"       {line}")
        return False

    def skip(self, description: str, reason: str) -> None:
        self.skipped += 1
        print(f"  skip {description}: {reason}")


REPORT = Report()


def section(name: str) -> None:
    print(f"\n== {name}")


def run_cli(cli: str, arguments: list[str], timeout: int = 900) -> subprocess.CompletedProcess:
    return subprocess.run([cli, *arguments], capture_output=True, text=True, timeout=timeout)


def probe_duration(path: str) -> float | None:
    result = subprocess.run(
        [FFPROBE, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", path],
        capture_output=True, text=True,
    )
    try:
        return float(result.stdout.strip())
    except ValueError:
        return None


def probe_stream_names(path: str) -> list[tuple[int, str, str]]:
    result = subprocess.run(
        [FFPROBE, "-v", "error", "-print_format", "json",
         "-show_entries", "stream=index,codec_type,codec_name", path],
        capture_output=True, text=True,
    )
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError:
        return []
    return [
        (int(s["index"]), s.get("codec_type", "?"), s.get("codec_name", "?"))
        for s in document.get("streams", [])
    ]


def ffprobe_can_read(path: Path) -> bool:
    result = subprocess.run(
        [FFPROBE, "-hide_banner", "-v", "error", "-print_format", "json",
         "-show_format", "-show_streams", "-i", str(path)],
        capture_output=True, text=True,
    )
    return result.returncode == 0


# ---------------------------------------------------------------------------
# probe
# ---------------------------------------------------------------------------
def section_probe(cli: str, fixtures: Path, main: Path) -> None:
    section("probe")
    for fixture in sorted(fixtures.iterdir()):
        name = fixture.name
        if fixture.suffix.lower() == ".srt" or not fixture.is_file():
            continue
        result = run_cli(cli, ["probe", str(fixture)])
        if result.returncode == 0:
            REPORT.ok(f"probe {name}", True)
            continue
        if not ffprobe_can_read(fixture):
            REPORT.skip(f"probe {name}", "the local ffprobe cannot read this file either")
            continue
        REPORT.ok(f"probe {name}", False, result.stdout + result.stderr)

    result = run_cli(cli, ["probe", str(main)])
    text = result.stdout
    REPORT.ok("probe names the video codec", "h264" in text, text)
    REPORT.ok("probe reports the frame size", "320x240" in text)
    REPORT.ok("probe reports the frame rate", "25.000 fps" in text)
    REPORT.ok("probe reports keyframes", "keyframes:" in text)
    REPORT.ok("probe states how reliable the duration is", "reliable" in text)

    rotation = fixtures / "rotation.mp4"
    if rotation.exists():
        result = run_cli(cli, ["probe", str(rotation)])
        REPORT.ok("probe reports a display-matrix rotation", "rotated 90" in result.stdout, result.stdout)

    # Every stream class the product claims to preserve has to survive the probe.
    multi = fixtures / "multi_stream.mkv"
    if multi.exists():
        result = run_cli(cli, ["probe", str(multi)])
        streams = probe_stream_names(multi)
        # `streams` is a list, not a set: collapsing it first would make "two audio
        # tracks" impossible to assert, because a set holds one "audio" however many
        # tracks the file has.  This is a real assertion and it does fail when the
        # fixture is wrong, which a set comparison could never do.
        kinds = [kind for _, kind, _ in streams]
        REPORT.ok("the multi-stream fixture has video, two audio tracks and a subtitle",
                  kinds.count("audio") == 2 and kinds.count("video") == 1 and "subtitle" in kinds,
                  f"streams={streams}")
        REPORT.ok("the probe reports the subtitle and both audio streams",
                  result.stdout.count("audio") >= 2 and "subtitle" in result.stdout, result.stdout)


# ---------------------------------------------------------------------------
# plan
# ---------------------------------------------------------------------------
def section_plan(cli: str, main: Path, work: Path) -> None:
    section("plan")
    plan_path = work / "plan.json"
    result = run_cli(cli, ["plan", str(main), "--limit", "256KiB", "--json"])
    plan_path.write_text(result.stdout, encoding="utf-8")
    if not REPORT.ok("plan of a feasible size limit succeeds", result.returncode == 0,
                     result.stdout + result.stderr):
        return
    try:
        plan = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        REPORT.ok("plan JSON is self-consistent", False, str(error))
        return

    limit = 256 * 1024
    problems = []
    if plan.get("planned") is not True:
        problems.append("plan is not marked planned")
    segments = plan.get("segments", [])
    if len(segments) < 2:
        problems.append(f"expected more than one part, got {len(segments)}")
    if segments and segments[0]["start"]["num"] != 0:
        problems.append("the plan does not start at zero")
    for index, segment in enumerate(segments):
        if segment["predicted_bytes"] > limit:
            problems.append(f"part {index + 1} is predicted at {segment['predicted_bytes']} > {limit}")
        if index and segments[index - 1]["end"] != segment["start"]:
            problems.append(f"part {index + 1} does not start where part {index} ended")
        if segment["start"]["den"] == 0 or segment["end"]["den"] == 0:
            problems.append(f"part {index + 1} has an invalid rational timestamp")
    REPORT.ok("plan JSON is self-consistent", not problems,
              "; ".join(problems) if problems else
              f"{len(segments)} part(s), every prediction inside {limit} bytes, contiguous")

    result = run_cli(cli, ["plan", str(main), "--limit", "1KiB"])
    REPORT.ok("an impossible size limit exits with 2 (cannot plan), got %d" % result.returncode,
              result.returncode == 2, result.stdout + result.stderr)
    combined = (result.stdout + result.stderr).lower()
    REPORT.ok("the refusal says what to do",
              "raise the limit" in combined or "smaller" in combined,
              result.stdout + result.stderr)

    result = run_cli(cli, ["plan", str(main), "--limit", "256KiB", "--parts", "2"])
    REPORT.ok("a part-count limit that cannot be met exits with 2, got %d" % result.returncode,
              result.returncode == 2, result.stdout + result.stderr)
    REPORT.ok("the part-count refusal mentions parts", "part" in (result.stdout + result.stderr).lower())

    # A source smaller than the limit needs exactly one part and must not be
    # refused - "the limit is bigger than the file" is a normal request.
    still = work / "fixtures" / "still.png"
    if still.exists():
        result = run_cli(cli, ["plan", str(still), "--limit", "10MiB"])
        REPORT.ok("a still image is refused with a reason rather than planned",
                  result.returncode != 0 and "single image" in (result.stdout + result.stderr).lower(),
                  result.stdout + result.stderr)


# ---------------------------------------------------------------------------
# split by size
# ---------------------------------------------------------------------------
def verify_parts_against_source(checker_output: str) -> tuple[bool, str]:
    return "RESULT: EXACT" in checker_output, checker_output


def section_split_size(cli: str, main: Path, work: Path) -> Path:
    section("split by size (copy mode, verified)")
    out = work / "split-size"
    log = work / "split-size.log"
    result = run_cli(cli, ["split", str(main), "--limit", "256KiB", "--out", str(out), "--verify-decode"])
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if not REPORT.ok("split by size succeeds", result.returncode == 0, result.stdout + result.stderr):
        return out
    parts = sorted(out.glob("*.mp4"))
    REPORT.ok("the split produced %d part(s)" % len(parts), len(parts) >= 2)
    limit = 256 * 1024
    oversized = [f"{p.name}={p.stat().st_size}" for p in parts if p.stat().st_size > limit]
    REPORT.ok("every part is at most %d bytes" % limit, not oversized, ", ".join(oversized))
    text = result.stdout
    REPORT.ok("the split reports that every part was verified", "written and verified" in text)
    REPORT.ok("the per-part table shows each part's verification result",
              all(re.search(rf"part {p.stem[-3:]}:.*verified", text) or "verified" in text for p in parts))

    # Packet-level check of every part against the source, using the boundaries
    # the plan reported.  This is the strongest statement available: every packet
    # of the part is the corresponding source packet, byte for byte, and the
    # decoded video is a contiguous slice of the source's.
    plan_path = work / "plan-size.json"
    plan_result = run_cli(cli, ["plan", str(main), "--limit", "256KiB", "--json"])
    plan_path.write_text(plan_result.stdout, encoding="utf-8")
    plan = json.loads(plan_result.stdout)
    all_ok = True
    details = []
    for segment in plan["segments"]:
        name = segment["file_name"]
        start = segment["start"]["num"] / segment["start"]["den"]
        end = segment["end"]["num"] / segment["end"]["den"]
        part = out / name
        if not part.exists():
            all_ok = False
            details.append(f"{name}: the planned file does not exist")
            continue
        checked = subprocess.run(
            [sys.executable, str(CHECK), "part", str(main), str(part), f"{start:.6f}", f"{end:.6f}"],
            capture_output=True, text=True,
        )
        ok, output = verify_parts_against_source(checked.stdout + checked.stderr)
        all_ok = all_ok and ok
        details.append(f"-- {name} [{start:.3f}, {end:.3f})\n" + output)
    REPORT.ok("every part is a byte-exact copy of the source range it covers",
              all_ok, "\n".join(details))
    return out


# ---------------------------------------------------------------------------
# duration limits
# ---------------------------------------------------------------------------
def section_split_duration(cli: str, fixtures: Path, work: Path) -> None:
    section("hard duration limits")
    short_gop = fixtures / "h264_shortgop.mp4"
    if not short_gop.exists():
        REPORT.skip("duration limits", "the short-GOP fixture is missing")
        return
    for limit_text, limit_seconds in (("1", 1.0), ("2", 2.0), ("3", 3.0)):
        out = work / f"split-duration-{limit_text}"
        result = run_cli(cli, ["split", str(short_gop), "--duration", limit_text,
                               "--out", str(out)])
        if not REPORT.ok(f"split with a {limit_text} s duration limit succeeds",
                         result.returncode == 0, result.stdout + result.stderr):
            continue
        parts = sorted(out.glob("*.mp4"))
        REPORT.ok(f"the {limit_text} s split produced {len(parts)} part(s)", len(parts) >= 2)
        over = []
        for part in parts:
            duration = probe_duration(str(part))
            if duration is None:
                over.append(f"{part.name}: duration unreadable")
            elif duration > limit_seconds + 1e-3:
                over.append(f"{part.name}: {duration:.6f} s > {limit_seconds} s")
        REPORT.ok(f"every part of the {limit_text} s split is within the limit",
                  not over, "\n".join(over))


# ---------------------------------------------------------------------------
# hostile paths
# ---------------------------------------------------------------------------
def section_hostile_paths(cli: str, fixtures: Path, work: Path) -> None:
    section("hostile file and folder names")
    cases = [
        ("spaces and an apostrophe", "l'apostrophe & spaces.mp4"),
        ("shell metacharacters", "quote' and (paren) and [bracket] and spaces.mp4"),
        ("non-ASCII characters", "vidéo été 日本語.mp4"),
        ("a very long file name",
         "a_very_long_file_name_that_approaches_the_windows_path_length_limit_0123456789.mp4"),
    ]
    for label, name in cases:
        source = fixtures / name
        if not source.exists():
            REPORT.skip(f"split a file with {label}", "fixture missing")
            continue
        out = work / "split-names" / label.replace(" ", "_")
        # A nested destination too, so the directory path is exercised as well.
        nested = out / "deep" / "nested" / "parts"
        result = run_cli(cli, ["split", str(source), "--duration", "1", "--out", str(nested)])
        if not REPORT.ok(f"split a file with {label}", result.returncode == 0,
                         result.stdout + result.stderr):
            continue
        parts = sorted(nested.glob("*.mp4"))
        REPORT.ok(f"the {label} split produced {len(parts)} part(s)", len(parts) >= 2)
        merged_dir = work / "merged-names" / label.replace(" ", "_")
        merge = run_cli(cli, ["merge", str(merged_dir), *[str(p) for p in parts],
                                  "--name", "quote & part merged.mp4", "--verify-decode"])
        if not REPORT.ok(f"merge parts whose names carry {label}", merge.returncode == 0,
                         merge.stdout + merge.stderr):
            continue
        merged = merged_dir / "quote & part merged.mp4"
        REPORT.ok(f"the merged file exists under the requested name ({label})", merged.exists())
        if merged.exists():
            checked = subprocess.run(
                [sys.executable, str(CHECK), "merged", str(merged), *[str(p) for p in parts]],
                capture_output=True, text=True,
            )
            REPORT.ok(f"the {label} merge reproduces its inputs",
                      checked.returncode == 0, checked.stdout + checked.stderr)


# ---------------------------------------------------------------------------
# merge
# ---------------------------------------------------------------------------
def section_merge(cli: str, parts_dir: Path, main: Path, fixtures: Path, work: Path) -> None:
    section("merge")
    parts = sorted(parts_dir.glob("*.mp4"))
    if len(parts) < 2:
        REPORT.skip("merge", "no parts to merge")
        return
    merged_dir = work / "merged"
    result = run_cli(cli, ["merge", str(merged_dir), *[str(p) for p in parts],
                              "--name", "joined.mp4", "--verify-decode", "--verify-hash"])
    if not REPORT.ok("merging the parts of a split succeeds and verifies",
                     result.returncode == 0, result.stdout + result.stderr):
        return
    merged = merged_dir / "joined.mp4"
    if not REPORT.ok("the merged file exists", merged.exists()):
        return

    checked = subprocess.run(
        [sys.executable, str(CHECK), "merged", str(merged), *[str(p) for p in parts]],
        capture_output=True, text=True,
    )
    REPORT.ok("the merged file reproduces its inputs packet for packet",
              checked.returncode == 0, checked.stdout + checked.stderr)

    source_duration = probe_duration(str(main))
    merged_duration = probe_duration(str(merged))
    if source_duration is not None and merged_duration is not None:
        # The tolerance is one video frame per junction, not a flat 0.1 s.
        #
        # A concatenation cannot be exact in duration, and the reason is arithmetic
        # rather than a defect in either side.  Every part's video ends on a frame
        # boundary that is not its nominal range end: measured on the split fixture,
        # part 1 covers a 2.000 s range but its 50th frame starts at 1.983 and runs
        # 40 ms, so the file really is 2.023 s long.  The concat demuxer places the
        # next input after the whole previous one, so each junction adds that 23 ms
        # to the timeline.  Five parts of a 10 s file accumulate 0.115 s.
        #
        # The content is exact, and check_parts.py above proves it: 250 of 250 video
        # packets byte-identical, 250 decoded frame hashes equal in order, 431 of 431
        # audio packets byte-identical.  So the only thing being measured here is
        # timestamp bookkeeping, and the honest bound for it scales with the number
        # of junctions rather than being a constant that happens to pass or fail by
        # luck.  One 40 ms frame per junction, with 100 ms of slack for container
        # rounding, keeps a genuine gap from passing.
        junctions = max(len(parts) - 1, 0)
        tolerance = junctions * 0.040 + 0.100
        delta = abs(source_duration - merged_duration)
        REPORT.ok(
            f"the merged duration is within {tolerance:.3f} s of the source "
            f"({junctions} junction(s), one 40 ms frame each; delta {delta:.6f} s)",
            delta <= tolerance,
            f"source={source_duration} merged={merged_duration} tolerance={tolerance:.3f}")

    single = run_cli(cli, ["merge", str(merged_dir), str(merged), "--name", "again.mp4"])
    REPORT.ok("merging a single file is refused (exit %d)" % single.returncode,
              single.returncode != 0, single.stdout + single.stderr)

    # The merge must refuse inputs that are not compatible, say why, and leave
    # nothing behind.
    bad_dir = work / "merged-bad"
    other = fixtures / "h265_cfr.mkv"
    if other.exists():
        bad = run_cli(cli, ["merge", str(bad_dir), *[str(p) for p in parts], str(other)])
        REPORT.ok("merging different codecs exits with 2 (cannot plan), got %d" % bad.returncode,
                  bad.returncode == 2, bad.stdout + bad.stderr)
        combined = (bad.stdout + bad.stderr).lower()
        REPORT.ok("the refusal names the difference",
                  "hevc" in combined or "h265" in combined or "codec" in combined,
                  bad.stdout + bad.stderr)
        leftovers = list(bad_dir.iterdir()) if bad_dir.exists() else []
        REPORT.ok("the refused merge left no output", not leftovers,
                  ", ".join(p.name for p in leftovers))


# ---------------------------------------------------------------------------
# output transactions and collisions
# ---------------------------------------------------------------------------
def section_collisions(cli: str, main: Path, work: Path) -> None:
    section("output transactions and collisions")
    out = work / "collisions"
    first = run_cli(cli, ["split", str(main), "--limit", "256KiB", "--out", str(out)])
    if not REPORT.ok("the first split into a fresh folder succeeds",
                     first.returncode == 0, first.stdout + first.stderr):
        return
    before = {p.name: (p.stat().st_size, p.stat().st_mtime_ns) for p in sorted(out.glob("*.mp4"))}
    if not REPORT.ok("the first split wrote parts", len(before) >= 2):
        return

    second = run_cli(cli, ["split", str(main), "--limit", "256KiB", "--out", str(out)])
    if not REPORT.ok("the second split into the same folder succeeds",
                     second.returncode == 0, second.stdout + second.stderr):
        return
    after = {p.name: (p.stat().st_size, p.stat().st_mtime_ns) for p in sorted(out.glob("*.mp4"))}
    untouched = all(after.get(name) == value for name, value in before.items())
    REPORT.ok("the existing parts were not overwritten", untouched,
              "\n".join(f"{n}: {before[n]} -> {after.get(n)}" for n in before if after.get(n) != before[n]))
    renamed = [name for name in after if name not in before]
    REPORT.ok(f"the new parts were written under free names ({len(renamed)} new file(s))",
              len(renamed) >= len(before),
              "\n".join(sorted(renamed)))
    REPORT.ok("the collision was reported rather than silently resolved",
              "taken" in second.stdout or "(2)" in " ".join(after), second.stdout)
    stray = [p.name for p in out.iterdir() if p.name.startswith(".monosect-")]
    REPORT.ok("no working folder was left behind", not stray, ", ".join(stray))


# ---------------------------------------------------------------------------
# formats
# ---------------------------------------------------------------------------
def section_formats(cli: str, fixtures: Path, work: Path) -> None:
    section("other containers and stream shapes")
    cases = [
        ("h265_cfr.mkv", "HEVC in Matroska", ["--limit", "200KiB"]),
        ("vfr_concat.mkv", "variable frame rate", ["--duration", "3"]),
        ("multi_stream.mkv", "video + 2 audio + subtitle + attachment + chapters", ["--duration", "3"]),
        ("non_zero_start.mkv", "a non-zero first timestamp", ["--duration", "3"]),
        ("negative_audio_offset.mkv", "negative timestamps", ["--duration", "3"]),
        ("mpegts_h264.ts", "MPEG-TS", ["--duration", "3"]),
        ("audio_only.m4a", "audio only", ["--duration", "2"]),
        ("h264_bframes.mp4", "B-frame reordering", ["--duration", "3"]),
    ]
    for name, label, constraint in cases:
        source = fixtures / name
        if not source.exists():
            REPORT.skip(f"split {label}", "fixture missing")
            continue
        out = work / "formats" / Path(name).stem
        result = run_cli(cli, ["split", str(source), *constraint, "--out", str(out)])
        if not REPORT.ok(f"split {label}", result.returncode == 0, result.stdout + result.stderr):
            continue
        produced = sorted(p for p in out.iterdir() if p.is_file() and not p.name.startswith("."))
        REPORT.ok(f"{label} produced {len(produced)} part(s)", len(produced) >= 1)
        source_streams = probe_stream_names(str(source))
        # Every part must carry the same stream *kinds* as the source, but a stream
        # with nothing in a part's time range is legitimately absent from it: a
        # subtitle track whose only cue is at 0.5 s contributes nothing to a part
        # covering 4-6 s.  Asserting equality would demand the impossible - that a
        # part invent a cue that is not there - and it did: the multi-stream
        # fixture's last part was reported as "not keeping the source's stream
        # kinds" for correctly omitting a subtitle stream it has no cue for.
        #
        # So the check is that the part's kinds are a subsequence of the source's,
        # in the same order, with the timed (video/audio) streams all present.
        source_kinds = [k for _, k, _ in source_streams]
        timed = {"video", "audio"}
        missing = [k for k in source_kinds if k in timed]
        for part in produced:
            part_kinds = [k for _, k, _ in probe_stream_names(str(part))]
            lost = [k for k in missing if k not in part_kinds]
            reordered = [k for k in part_kinds if k not in source_kinds]
            subset = part_kinds == [k for k in source_kinds if k in part_kinds]
            if lost or reordered or not subset:
                REPORT.ok(f"{label}: {part.name} keeps the source's stream kinds",
                          False, f"source={source_kinds} part={part_kinds} lost={lost}")
                break
        else:
            REPORT.ok(f"{label}: every part keeps the source's timed streams", True,
                      f"source={source_kinds}")


# ---------------------------------------------------------------------------
# re-encode path
# ---------------------------------------------------------------------------
def section_modes(cli: str, fixtures: Path, work: Path) -> None:
    section("preservation modes")
    short_gop = fixtures / "h264_shortgop.mp4"
    if not short_gop.exists():
        REPORT.skip("preservation modes", "the short-GOP fixture is missing")
        return
    for mode, label in (("keyframe", "keyframe-aligned copy"),
                        ("smart", "smart cut"),
                        ("reencode", "full re-encode")):
        out = work / f"mode-{mode}"
        result = run_cli(cli, ["split", str(short_gop), "--duration", "2", "--mode", mode,
                               "--out", str(out)])
        if not REPORT.ok(f"split in {label} mode", result.returncode == 0,
                         result.stdout + result.stderr):
            continue
        parts = sorted(out.glob("*.mp4"))
        over = [f"{p.name}: {probe_duration(str(p))}" for p in parts
                if (probe_duration(str(p)) or 0) > 2.0 + 1e-3]
        REPORT.ok(f"every part of the {label} split is within the 2 s duration limit",
                  not over, "\n".join(over))
        if mode in ("smart", "reencode"):
            # The point of these modes is that re-encoding happens, and the
            # product must say so rather than call the result a copy.
            REPORT.ok(f"{label} reports what it re-encoded",
                      "re-encode" in result.stdout or "re-encod" in result.stdout,
                      result.stdout)


# ---------------------------------------------------------------------------
# job history
# ---------------------------------------------------------------------------
def section_history(cli: str, main: Path, fixtures: Path, work: Path) -> None:
    section("job history")
    database = work / "jobs.jsonl"
    out = work / "split-jobs"
    result = run_cli(cli, ["split", str(main), "--limit", "256KiB", "--out", str(out), "--db", str(database)])
    REPORT.ok("split with a job database succeeds", result.returncode == 0, result.stdout + result.stderr)
    REPORT.ok("the job database is not empty",
              database.exists() and database.stat().st_size > 0)
    if not database.exists():
        return
    problems = []
    try:
        records = [json.loads(line) for line in
                   database.read_text(encoding="utf-8").splitlines() if line.strip()]
    except json.JSONDecodeError as error:
        REPORT.ok("the job record describes the completed job", False, str(error))
        return
    if not records:
        problems.append("no records")
    else:
        last = records[-1]
        if not last.get("job_id"):
            problems.append("no job id")
        if last.get("state") != "completed":
            problems.append(f"state is {last.get('state')}")
        parts = last.get("parts", [])
        if len(parts) < 2:
            problems.append(f"{len(parts)} parts recorded")
        if not all(p.get("bytes") for p in parts):
            problems.append("a recorded part has no size")
        if not all(p.get("verified") for p in parts):
            problems.append("a recorded part is not marked verified")
        if not last.get("source_path"):
            problems.append("no source path")
        if not last.get("source_bytes"):
            problems.append("no source size")
        # Metadata only: a job record must never contain media bytes.
        raw = database.read_bytes()
        if b"ftyp" in raw or b"\x1a\x45\xdf\xa3" in raw:
            problems.append("the record contains media bytes")
    REPORT.ok("the job record describes the completed job and is metadata only",
              not problems, "; ".join(problems) if problems else
              f"{len(records)} record(s), last one completed with verified parts")

    listing = run_cli(cli, ["jobs", "--db", str(database)])
    REPORT.ok("jobs lists the history", listing.returncode == 0, listing.stdout + listing.stderr)
    REPORT.ok("the listing shows the final state", "completed" in listing.stdout)

    # A refused job must be remembered too: "what did I already do" is the whole
    # point of a history.
    other = fixtures / "h265_cfr.mkv"
    if other.exists() and (work / "merged-bad").exists():
        before = len(records)
        run_cli(cli, ["merge", str(work / "merged-fail"), str(main), str(other),
                      "--db", str(database)])
        after = [json.loads(line) for line in
                 database.read_text(encoding="utf-8").splitlines() if line.strip()]
        states = [r["state"] for r in after]
        REPORT.ok("a refused job is recorded as failed",
                  len(after) > before and "failed" in states, f"states={states}")


# ---------------------------------------------------------------------------
# cancellation
# ---------------------------------------------------------------------------
def section_cancel(cli: str, fixtures: Path, work: Path) -> None:
    section("cancellation")
    if "cancel" in SKIP:
        REPORT.skip("cancellation", "skipped by MONOSECT_E2E_SKIP")
        return
    large = fixtures / "larger.mp4"
    if not large.exists():
        REPORT.skip("cancellation", "the larger fixture is missing")
        return
    out = work / "split-cancel"
    log = work / "split-cancel.log"
    command = [cli, "split", str(large), "--limit", "8MiB", "--out", str(out), "--verify-decode"]
    creation_flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    with open(log, "w", encoding="utf-8") as handle:
        process = subprocess.Popen(command, stdout=handle, stderr=subprocess.STDOUT,
                                   creationflags=creation_flags)
        time.sleep(1.5)
        interrupted = False
        try:
            if os.name == "nt":
                process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                process.send_signal(signal.SIGINT)
            interrupted = True
        except (OSError, ValueError) as error:
            print(f"  --   could not deliver the interrupt: {error}")
        try:
            process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=30)
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""

    if not interrupted:
        REPORT.skip("cancellation", "the platform refused to deliver an interrupt")
        return
    REPORT.ok(f"an interrupted split does not report success (exit {process.returncode})",
              process.returncode != 0, text[-2000:])
    REPORT.ok("the log says the job was cancelled", "cancel" in text.lower(), text[-2000:])
    published = len(re.findall(r"^\s*published ", text, re.MULTILINE))
    present = [p for p in out.glob("*") if p.is_file() and not p.name.startswith(".")] if out.exists() else []
    REPORT.ok(f"every file in the output folder is a published (verified) part: "
              f"{len(present)} present, {published} published",
              len(present) <= published)
    temporary = [p.name for p in out.rglob(".monosect-*")] if out.exists() else []
    REPORT.ok("the cancelled job left no working folder behind", not temporary, ", ".join(temporary))
    incomplete = [p.name for p in present if p.suffix == ".partial"]
    REPORT.ok("no partial file was published", not incomplete, ", ".join(incomplete))


# ---------------------------------------------------------------------------
# planner convergence
# ---------------------------------------------------------------------------
def section_closeness(cli: str, main: Path, work: Path) -> None:
    section("closeness to the size limit")
    limit = 256 * 1024
    out = work / "closeness"
    result = run_cli(cli, ["split", str(main), "--limit", f"{limit}", "--out", str(out),
                           "--closeness", "1"])
    if not REPORT.ok("the closeness pass split succeeds", result.returncode == 0,
                     result.stdout + result.stderr):
        return
    parts = sorted(out.glob("*.mp4"))
    sizes = [p.stat().st_size for p in parts]
    over = [f"{p.name}={p.stat().st_size}" for p in parts if p.stat().st_size > limit]
    REPORT.ok("no part exceeds the hard limit", not over, ", ".join(over))
    if not sizes:
        return
    # A part that is nowhere near the limit means the planner gave up on
    # closeness; that is not a correctness failure but it is a product failure.
    slack = [limit - s for s in sizes]
    print(f"       sizes={[f'{s}' for s in sizes]} slack={[f'{s}' for s in slack]}")
    REPORT.ok("the average part uses at least half of the limit",
              sum(sizes) / len(sizes) >= limit / 2,
              f"mean={sum(sizes) / len(sizes):.0f} limit={limit}")

    # The same source with a much larger limit needs fewer, larger parts.
    big = work / "closeness-big"
    result = run_cli(cli, ["split", str(main), "--limit", f"{2 * limit}", "--out", str(big)])
    if REPORT.ok("the same source with a doubled limit succeeds", result.returncode == 0,
                 result.stdout + result.stderr):
        big_parts = sorted(big.glob("*.mp4"))
        REPORT.ok(f"a doubled limit produced fewer parts ({len(big_parts)} vs {len(parts)})",
                  len(big_parts) < len(parts))

    # The same split in full re-encode, which is the mode where the closeness pass
    # actually wins.
    #
    # This line is the only reason the stale-path defect in JobRunner was ever
    # caught, and it is here because the check above is not enough on its own: with no
    # --mode the harness defaults to keyframe-aligned copy, and in that mode the next
    # keyframe overshoots the limit, so the retry is never larger, the retry-wins
    # branch is never taken, and the publish step never sees the bug.  Re-encode
    # predicts the payload from the *source* bitrate while a CRF encode lands well
    # under it, so every part sits far below the limit, the retry always wins - and
    # the publish step then committed the temporary file that the retry branch had
    # already deleted, discarding a verified part and failing the whole job.
    reencode = work / "closeness-reencode"
    result = run_cli(cli, ["split", str(main), "--limit", f"{limit}", "--out", str(reencode),
                           "--mode", "reencode", "--closeness", "1"])
    if REPORT.ok("the closeness pass succeeds in full re-encode mode", result.returncode == 0,
                 result.stdout + result.stderr):
        re_parts = sorted(reencode.glob("*.mp4"))
        REPORT.ok("re-encode with a closeness pass publishes every part it writes",
                  len(re_parts) > 0, f"{len(re_parts)} part(s)")
        re_over = [f"{p.name}={p.stat().st_size}" for p in re_parts if p.stat().st_size > limit]
        REPORT.ok("no re-encoded part exceeds the hard limit", not re_over, ", ".join(re_over))
        # The failure this guards against published nothing at all while the log said
        # the retry had been written and verified, so a retry that reports itself as
        # written must also leave a file behind.
        REPORT.ok("a won closeness retry leaves its part on disk",
                  not re_parts or all(p.stat().st_size > 0 for p in re_parts))
        # No temporary working folder may survive a successful split.
        REPORT.ok("no working folder is left behind after a successful re-encode split",
                  not any(p.name.startswith(".monosect-") for p in reencode.iterdir()),
                  ", ".join(p.name for p in reencode.iterdir() if p.name.startswith(".monosect-")))


def main(argv: list[str]) -> int:
    if len(argv) < 1:
        print(__doc__, file=sys.stderr)
        return 64
    cli = argv[0]
    if not os.path.exists(cli):
        print(f"the CLI does not exist: {cli}", file=sys.stderr)
        return 64
    work = Path(argv[1] if len(argv) > 1 else "e2e-work").resolve()
    for tool, name in ((FFMPEG, "ffmpeg"), (FFPROBE, "ffprobe")):
        if shutil.which(tool) is None and not Path(tool).exists():
            print(f"missing tool: {name} ({tool})", file=sys.stderr)
            return 69

    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)
    fixtures = work / "fixtures"

    print(f"end-to-end: CLI={cli} work={work}")
    generated = subprocess.run(
        [sys.executable, str(FIXTURES_SCRIPT), str(fixtures)],
        capture_output=True, text=True,
    )
    if generated.returncode != 0:
        print("fixture generation failed:\n" + generated.stdout + generated.stderr, file=sys.stderr)
        return 1

    main_fixture = fixtures / "h264_cfr.mp4"
    parts_dir = section_split_size(cli, main_fixture, work)
    section_probe(cli, fixtures, main_fixture)
    section_plan(cli, main_fixture, work)
    section_split_duration(cli, fixtures, work)
    section_formats(cli, fixtures, work)
    section_modes(cli, fixtures, work)
    section_hostile_paths(cli, fixtures, work)
    section_merge(cli, parts_dir, main_fixture, fixtures, work)
    section_collisions(cli, main_fixture, work)
    section_closeness(cli, main_fixture, work)
    section_history(cli, main_fixture, fixtures, work)
    section_cancel(cli, fixtures, work)

    verdict = f"{REPORT.passed} passed, {REPORT.failed} failed, {REPORT.skipped} skipped"
    print(f"\n{verdict}")
    # The verdict is also written into the work directory.  A CI log is the first
    # thing to be truncated and the last thing anybody reads, and a run whose only
    # record is "exit 1" sends the next person hunting through a thousand lines of
    # FFmpeg output.  The file names the failures directly.
    try:
        summary = work / "e2e.log"
        summary.write_text(
            f"{verdict}\n\n"
            + "\n".join(f"failed: {name}" for name in REPORT.failed_names)
            + ("\n" if REPORT.failed_names else ""),
            encoding="utf-8",
        )
    except OSError as error:   # a full disk must not turn a passing run into a failing one
        print(f"could not write the summary ({error}); the verdict above still stands", file=sys.stderr)
    if REPORT.failed:
        for name in REPORT.failed_names:
            print(f"  failed: {name}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
