#!/usr/bin/env python3
"""MonoSect — synthetic media fixtures (TODO 235).

Every file used by the end-to-end tests is generated here from FFmpeg's own
synthetic sources ("testsrc2", "sine", "color").  Nothing is downloaded and
nothing is committed.

Why Python and not the bash script this replaces: bash is not part of Windows.
A verification suite that can only run on Linux proves nothing about the platform
the product ships on, and on the developer's own Windows machine it simply could
not be executed at all.  The commands are the same; only the shell is gone.

Two properties are kept:

  * **Determinism.**  Every input is synthesized from a fixed pattern with fixed
    parameters, so the same FFmpeg build reproduces byte-identical fixtures and a
    failing test can be replayed.  A different FFmpeg build may produce
    different bytes - that is normal for any encoder - which is why the tests
    assert *relationships* between files (this part is a subrange of that part,
    this merge is the concatenation of those parts) and never recorded hashes of
    the fixtures themselves.
  * **Coverage of what breaks naive implementations.**  B-frame reordering, short
    and long GOPs, HEVC, variable frame rate, several streams including a subtitle
    and an attachment, chapters, a non-zero first timestamp, a stream that starts
    before zero, an audio-only file, a still image, a transport stream, rotated
    video, files large enough to need several parts, and hostile file names.

Usage:
    make_fixtures.py [output-directory] [--force] [--list]

Environment:
    MONOSECT_FFMPEG   path to ffmpeg (default: ffmpeg from PATH)
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

FFMPEG = os.environ.get("MONOSECT_FFMPEG", "ffmpeg")

# name -> description.  The generator switches on the name.
FIXTURES = [
    ("h264_cfr.mp4", "H.264 CFR + AAC, 10 s, 25 fps, keyframe every 2 s (the main split fixture)"),
    ("h264_shortgop.mp4", "H.264 with a 0.2 s GOP - many keyframes, so size limits are met closely"),
    ("h264_bframes.mp4", "H.264 with B-frames and reordering (dts != pts)"),
    ("h265_cfr.mkv", "H.265/HEVC in Matroska"),
    ("vfr_concat.mkv", "variable frame rate (24 fps joined with 30 fps)"),
    ("multi_stream.mkv", "video + two audio tracks + subtitle + attachment + chapters"),
    ("audio_only.m4a", "AAC only, no video stream"),
    ("non_zero_start.mkv", "first video timestamp is 5 s, not 0"),
    ("negative_audio_offset.mkv", "audio starts 0.5 s before the video (negative timestamps)"),
    ("rotation.mp4", "video with a 90 degree display matrix"),
    ("mpegts_h264.ts", "H.264 + AAC in MPEG-TS"),
    ("still.png", "single still image"),
    ("subtitles.srt", "subtitle sidecar for the multi-stream fixture"),
    ("larger.mp4", "~40 MB H.264, so a small byte limit needs several parts"),
    ("l'apostrophe & spaces.mp4", "name with a quote, an ampersand and spaces"),
    ("quote' and (paren) and [bracket] and spaces.mp4", "name with shell-hostile characters"),
    ("vidéo été 日本語.mp4", "non-ASCII name"),
    (
        "a_very_long_file_name_that_approaches_the_windows_path_length_limit_0123456789.mp4",
        "long name",
    ),
]

# Names that are only content for another fixture, never probed on their own.
SIDECARS = {"subtitles.srt"}

V = "testsrc2=size=320x240:rate=25:duration=10"
V6 = "testsrc2=size=320x240:rate=25:duration=6"
TONE = "sine=frequency=440:sample_rate=44100:duration=10"

CHAPTERS = """;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=0
END=2000
title=Opening

[CHAPTER]
TIMEBASE=1/1000
START=2000
END=6000
title=Second chapter
"""

SUBTITLES = """1
00:00:00,500 --> 00:00:02,000
MonoSect fixture subtitle

2
00:00:02,500 --> 00:00:05,000
Second cue
"""

# Fixtures that are 3 s, 160x120, 10 fps clips under a hostile name.  Generated
# with the same encoder settings so their encoded content is comparable.
HOSTILE_NAMES = {
    "l'apostrophe & spaces.mp4",
    "quote' and (paren) and [bracket] and spaces.mp4",
    "vidéo été 日本語.mp4",
    "a_very_long_file_name_that_approaches_the_windows_path_length_limit_0123456789.mp4",
}


def run(arguments: list[str]) -> None:
    result = subprocess.run(arguments, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(
            f"ffmpeg failed ({result.returncode}) for:\n  {' '.join(arguments)}\n{result.stderr}"
        )


def ffmpeg_run(arguments: list[str]) -> None:
    run([FFMPEG, "-hide_banner", "-nostdin", "-loglevel", "error", "-y", *arguments])


def generate(name: str, target: Path, work: Path, fixtures_dir: Path) -> None:
    if name in SIDECARS:
        target.write_text(SUBTITLES, encoding="utf-8")
        return

    # `fixtures_dir` is the directory the fixtures will finally live in.  The
    # multi-stream fixture *reads* the subtitle sidecar from there, so it has to be
    # told where that is rather than inferring it from its own output path - which
    # during staging points at the .incomplete folder.
    if name in HOSTILE_NAMES:
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=10:duration=3",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                "-g", "10", "-an", str(target),
            ]
        )
        return

    if name == "h264_cfr.mp4":
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V,
                "-f", "lavfi", "-i", TONE,
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                "-g", "50", "-keyint_min", "50", "-sc_threshold", "0",
                "-c:a", "aac", "-b:a", "96k", "-ac", "1",
                "-movflags", "+faststart", str(target),
            ]
        )
    elif name == "h264_shortgop.mp4":
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V6,
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                "-g", "5", "-keyint_min", "5", "-sc_threshold", "0", "-an", str(target),
            ]
        )
    elif name == "h264_bframes.mp4":
        # bf=2 with enough frames forces out-of-order packets, which is what the
        # packet index has to survive and what a naive `-ss` copy gets wrong.
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V,
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                "-g", "25", "-bf", "2", "-an", str(target),
            ]
        )
    elif name == "h265_cfr.mkv":
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V6,
                "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=44100:duration=6",
                "-c:v", "libx265", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                "-x265-params", "keyint=50:min-keyint=50:scenecut=0",
                "-c:a", "aac", "-b:a", "96k", "-ac", "1", str(target),
            ]
        )
    elif name == "vfr_concat.mkv":
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=24:duration=4",
                "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=30:duration=4",
                "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                # A keyframe every second.  Without this the fixture has exactly
                # ONE keyframe: libx264's default GOP is 250 frames, longer than the
                # whole clip, so there is nowhere to cut and the fixture tests "a file
                # with no boundaries" rather than variable frame rate.  With -g 24 the
                # two halves still have different frame intervals - which is what makes
                # it VFR - and the planner has keyframes to work from.
                "-g", "24",
                "-fps_mode", "vfr", "-an", str(target),
            ]
        )
    elif name == "multi_stream.mkv":
        chapters = work / "chapters.txt"
        chapters.write_text(CHAPTERS, encoding="utf-8")
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V6,
                "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=6",
                "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=6",
                "-i", str(fixtures_dir / "subtitles.srt"),
                "-f", "ffmetadata", "-i", str(chapters),
                "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "50",
                "-c:a:0", "aac", "-b:a:0", "96k", "-ac:a:0", "1",
                "-c:a:1", "ac3", "-b:a:1", "96k", "-ac:a:1", "1",
                "-c:s", "srt",
                "-metadata", "title=MonoSect multi-stream fixture",
                "-metadata", "artist=MonoSect tests",
                "-metadata:s:a:0", "language=eng",
                "-metadata:s:a:1", "language=deu",
                "-metadata:s:s:0", "language=eng",
                "-attach", str(fixtures_dir / "subtitles.srt"),
                "-metadata:s:t", "mimetype=application/x-subrip",
                "-map_metadata", "4", "-map_chapters", "4",
                str(target),
            ]
        )
    elif name == "audio_only.m4a":
        ffmpeg_run(
            ["-f", "lavfi", "-i", TONE, "-c:a", "aac", "-b:a", "96k", "-ac", "1", str(target)]
        )
    elif name == "non_zero_start.mkv":
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V6, "-output_ts_offset", "5",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                "-g", "50", "-an", str(target),
            ]
        )
    elif name == "negative_audio_offset.mkv":
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V6,
                "-itsoffset", "-0.5", "-f", "lavfi",
                "-i", "sine=frequency=440:sample_rate=44100:duration=6",
                "-map", "0:v", "-map", "1:a",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "50",
                "-c:a", "aac", "-b:a", "96k", "-ac", "1",
                "-copyts", "-avoid_negative_ts", "disabled", str(target),
            ]
        )
    elif name == "rotation.mp4":
        # A real display matrix.  The old "rotate" metadata tag is deprecated and
        # dropped by current FFmpeg, and -display_rotation only applies to a real
        # input, so the clip is encoded first and then remuxed with the matrix
        # applied (side data survives a stream copy).
        base = work / "rotation_base.mp4"
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V6,
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                "-an", str(base),
            ]
        )
        ffmpeg_run(["-display_rotation", "90", "-i", str(base), "-c", "copy", str(target)])
    elif name == "mpegts_h264.ts":
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", V6,
                "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=6",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "50",
                "-c:a", "aac", "-b:a", "96k", "-ac", "1", "-f", "mpegts", str(target),
            ]
        )
    elif name == "still.png":
        ffmpeg_run(
            ["-f", "lavfi", "-i", "color=c=red:size=320x240:duration=1", "-frames:v", "1", str(target)]
        )
    elif name == "larger.mp4":
        # Big enough that a 256 KiB limit needs many parts, small enough that
        # generating it stays under a few seconds.
        ffmpeg_run(
            [
                "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=60",
                "-f", "lavfi", "-i", "sine=frequency=300:sample_rate=48000:duration=60",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "60",
                "-b:v", "4000k", "-maxrate", "6000k", "-bufsize", "8000k",
                "-c:a", "aac", "-b:a", "128k", "-ac", "2", str(target),
            ]
        )
    else:
        raise SystemExit(f"no generator for fixture {name}")


def main(argv: list[str]) -> int:
    directory: str | None = None
    force = False
    for argument in argv:
        if argument == "--force":
            force = True
        elif argument == "--list":
            for name, description in FIXTURES:
                print(f"{name}\t{description}")
            return 0
        elif argument.startswith("-"):
            print(f"unknown option: {argument}", file=sys.stderr)
            return 64
        else:
            directory = argument

    target_dir = Path(directory or os.environ.get("MONOSECT_FIXTURES", "fixtures")).resolve()
    target_dir.mkdir(parents=True, exist_ok=True)
    # Fixtures are built under a dot-prefixed name and moved into place only when
    # complete, so an interrupted run never leaves a half-written fixture that a
    # later test would happily use.
    work = target_dir / ".incomplete"
    if work.exists():
        for leftover in work.iterdir():
            leftover.unlink()
    work.mkdir(exist_ok=True)

    # The subtitle sidecar is an input to the multi-stream fixture.
    subtitles = target_dir / "subtitles.srt"
    if force or not subtitles.exists():
        generate("subtitles.srt", work / "subtitles.srt", work, target_dir)
        (work / "subtitles.srt").replace(subtitles)

    generated = 0
    skipped = 0
    try:
        for name, _ in FIXTURES:
            if name in SIDECARS:
                continue
            final = target_dir / name
            if not force and final.exists():
                skipped += 1
                continue
            temporary = work / name
            generate(name, temporary, work, target_dir)
            temporary.replace(final)
            generated += 1
    finally:
        # The staging folder is removed whether or not generation succeeded: a
        # leftover .incomplete directory is itself a trap, because the next run
        # treats its contents as deletable leftovers and a partial fixture inside
        # it looks like a real one to anything that globs the directory.
        if work.exists():
            for leftover in work.iterdir():
                if leftover.is_file():
                    leftover.unlink()
            work.rmdir()
    print(f"fixtures: {generated} generated, {skipped} already present, directory {target_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
