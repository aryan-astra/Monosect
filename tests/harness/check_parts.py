#!/usr/bin/env python3
"""Independent checks of MonoSect's outputs, using only ffprobe and ffmpeg.

This is deliberately *not* the product's verifier: it re-probes the files and
compares them against the sources with FFmpeg alone, so a bug in MonoSect's own
verification code cannot hide itself.

Subcommands
-----------
part <source> <part> <start-seconds> <end-seconds>
    Every packet of the part must be a packet of the source, in the same order:
    same count, same size, same keyframe flag, the same bytes, and the run must
    sit inside the declared time window.  The part's decoded video must be a
    contiguous slice of the source's decoded video.

merged <merged> <input> [<input>...]
    The merged file must carry the same streams as the inputs, its packet count
    per stream must be exactly the sum of the inputs', and its packets must be
    the inputs' packets in order and byte for byte.  The only permitted
    difference is that the first packet of an input may carry additional NAL
    units, and those must be parameter sets or SEI - the redundant in-band copies
    a concat demuxer writes.  Anything else is a failure, not an excuse.  The
    decoded video frame hashes must equal the inputs' hashes concatenated.

limit <file> <max-bytes>
    The file must not be larger than the limit.  An exact byte comparison.

time <file> <max-seconds>
    The container duration must not be longer than the limit.

Exit code 0 means every check passed; 1 means a check failed (details on stdout).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys

FFPROBE = os.environ.get("MONOSECT_FFPROBE", "ffprobe")
FFMPEG = os.environ.get("MONOSECT_FFMPEG", "ffmpeg")

# The only tolerance used anywhere here.  A copied part's container duration can
# fall short of the requested end by one packet, and FFmpeg rounds a copy
# boundary to the stream's own grid.
TIME_TOLERANCE_S = 1e-3

# NAL unit types that carry no decoded samples, so a copy of them changes nothing
# about the pictures.  H.264: 6 SEI, 7 SPS, 8 PPS.  HEVC: 32 VPS, 33 SPS, 34 PPS,
# 39 prefix SEI, 40 suffix SEI.
PARAMETER_SET_TYPES = {6, 7, 8, 32, 33, 34, 39, 40}


def run(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(command)}\n{result.stderr}")
    return result.stdout


def probe(path: str, entries: str) -> dict:
    return json.loads(run([FFPROBE, "-v", "error", "-show_entries", entries, "-of", "json", path]))


def packets_of(path: str) -> dict[int, list[dict]]:
    """Packet list per stream index, in decode order as ffprobe reports it."""
    data = json.loads(
        run(
            [
                FFPROBE, "-v", "error", "-show_packets",
                "-show_entries",
                "packet=stream_index,pts,pts_time,dts,dts_time,duration,duration_time,size,pos,flags",
                "-of", "json", path,
            ]
        )
    )
    per_stream: dict[int, list[dict]] = {}
    for packet in data.get("packets", []):
        per_stream.setdefault(int(packet["stream_index"]), []).append(packet)
    return per_stream


def stream_info(path: str) -> dict[int, dict]:
    data = probe(path, "stream=index,codec_type,codec_name")
    return {int(s["index"]): s for s in data.get("streams", [])}


def read_bytes(path: str, position, size) -> bytes:
    with open(path, "rb") as handle:
        handle.seek(int(position))
        return handle.read(int(size))


def framemd5(path: str, stream: str = "0:v:0") -> list[str]:
    """Decoded frame hashes, one per line, comment lines removed."""
    text = run([FFMPEG, "-v", "error", "-nostdin", "-i", path, "-map", stream, "-f", "framemd5", "-"])
    hashes = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # "stream, dts, pts, duration, size, hash" - only the hash identifies the
        # decoded samples; the timestamps of a merged file legitimately differ.
        hashes.append(line.rsplit(",", 1)[-1].strip())
    return hashes


# ---------------------------------------------------------------------------
# NAL handling, for the one permitted junction difference
# ---------------------------------------------------------------------------
def nal_units(data: bytes) -> list[tuple[int, bytes]] | None:
    """Splits a packet into (type, payload) NAL units.

    Handles both container forms MonoSect writes and copies: Annex-B start codes
    (Matroska, MPEG-TS) and the 4-byte length prefixes of ISO base media (MP4).
    Returns None when the packet is not in either form, which makes the caller
    treat the packet as "not comparable" instead of guessing.
    """
    # Annex-B: look for a start code that is not preceded by another one.
    for index in range(0, len(data) - 3):
        if data[index] == 0 and data[index + 1] == 0 and \
                (data[index + 2] == 1 or (data[index + 2] == 0 and data[index + 3] == 1)):
            prefix = 3 if data[index + 2] == 1 else 4
            start = index + prefix
            body = data[start:]
            if _looks_like_nals(body):
                return _split_annexb(body)
    if _looks_like_length_prefixed(data):
        return _split_length_prefixed(data)
    return None


def _looks_like_nals(body: bytes) -> bool:
    offset = 0
    count = 0
    while offset + 3 < len(body):
        if body[offset] == 0 and body[offset + 1] == 0 and \
                (body[offset + 2] == 1 or (body[offset + 2] == 0 and body[offset + 3] == 1)):
            offset += 3 if body[offset + 2] == 1 else 4
            count += 1
            continue
        offset += 1
    return count > 0


def _split_annexb(body: bytes) -> list[tuple[int, bytes]]:
    starts = []
    offset = 0
    while offset + 3 < len(body):
        if body[offset] == 0 and body[offset + 1] == 0 and \
                (body[offset + 2] == 1 or (body[offset + 2] == 0 and body[offset + 3] == 1)):
            starts.append((offset, 3 if body[offset + 2] == 1 else 4))
            offset += 3 if body[offset + 2] == 1 else 4
            continue
        offset += 1
    units = []
    for index, (start, prefix) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(body)
        unit = body[start + prefix:end]
        if unit:
            units.append((unit[0] & 0x1F, unit))
    return units


def _looks_like_length_prefixed(data: bytes) -> bool:
    offset = 0
    count = 0
    while offset + 4 <= len(data):
        length = int.from_bytes(data[offset:offset + 4], "big")
        offset += 4
        if length <= 0 or offset + length > len(data):
            return False
        offset += length
        count += 1
    return offset == len(data) and count > 0


def _split_length_prefixed(data: bytes) -> list[tuple[int, bytes]]:
    units = []
    offset = 0
    while offset + 4 <= len(data):
        length = int.from_bytes(data[offset:offset + 4], "big")
        offset += 4
        unit = data[offset:offset + length]
        if unit:
            units.append((unit[0] & 0x1F, unit))
        offset += length
    return units


def differs_only_by_parameter_sets(actual: bytes, expected: bytes) -> bool:
    """True when `actual` is `expected` plus extra NAL units that decode to nothing.

    Every unit of `expected` must still be present in `actual` (as an identical
    byte string), and every unit of `actual` that is not in `expected` must be a
    parameter set or SEI.  A packet that differs in any other way fails.
    """
    actual_units = nal_units(actual)
    expected_units = nal_units(expected)
    if actual_units is None or expected_units is None:
        return False
    remaining = [payload for _, payload in actual_units]
    for _, payload in expected_units:
        try:
            remaining.remove(payload)
        except ValueError:
            return False
    for payload in remaining:
        if (payload[0] & 0x1F) not in PARAMETER_SET_TYPES:
            return False
    return True


# ---------------------------------------------------------------------------
# part
# ---------------------------------------------------------------------------
def command_part(source: str, part: str, start: str, end: str) -> int:
    start_seconds = float(start)
    end_seconds = float(end)
    source_packets = packets_of(source)
    part_packets = packets_of(part)
    info = stream_info(source)
    ok = True

    for index in sorted(part_packets):
        got = part_packets[index]
        codec = info.get(index, {}).get("codec_name", "?")
        stream_packets = source_packets.get(index, [])
        payload = sum(int(p["size"]) for p in got)

        # Locate the part's packet run inside the source's packet run.  Matching on
        # the bytes rather than on timestamps is what makes this check
        # independent of MonoSect's own idea of where the boundary was.
        offsets = [int(p["pos"]) for p in stream_packets if p.get("pos") is not None]
        matches: list[int] = []
        found = False
        for start_offset in range(0, max(0, len(stream_packets) - len(got) + 1)):
            window = stream_packets[start_offset:start_offset + len(got)]
            identical = True
            for expected, actual in zip(window, got):
                if int(expected["size"]) != int(actual["size"]):
                    identical = False
                    break
                if expected.get("flags") != actual.get("flags"):
                    identical = False
                    break
                if expected.get("pos") is None or actual.get("pos") is None:
                    continue
                if read_bytes(source, expected["pos"], expected["size"]) != \
                        read_bytes(part, actual["pos"], actual["size"]):
                    identical = False
                    break
            if identical:
                matches = list(range(start_offset, start_offset + len(got)))
                found = True
                break

        if not found:
            print(f"  stream {index} ({codec}): the part's {len(got)} packets are not a contiguous, "
                  f"byte-identical run of the source's {len(stream_packets)}")
            ok = False
            continue

        first_time = stream_packets[matches[0]].get("pts_time")
        last_time = stream_packets[matches[-1]].get("pts_time")
        window_note = ""
        if first_time is not None:
            low = start_seconds - TIME_TOLERANCE_S
            high = end_seconds + TIME_TOLERANCE_S
            if float(first_time) < low or float(last_time) >= high:
                window_note = (f"  !! the run lies at [{first_time}, {last_time}] which is outside the "
                               f"declared window [{start_seconds}, {end_seconds})")
                ok = False
        print(f"  stream {index} ({codec}): {len(got)} packet(s), {payload} bytes, byte-identical to the "
              f"source run at packet {matches[0]}{window_note}")

    # Decoded video: the part's frames must be a contiguous slice of the source's.
    if any(s.get("codec_type") == "video" for s in info.values()):
        source_frames = framemd5(source)
        part_frames = framemd5(part)
        if not part_frames:
            print("  video decoded frames: the part has no decodable video frames")
            ok = False
        else:
            contiguous = any(
                source_frames[i:i + len(part_frames)] == part_frames
                for i in range(0, len(source_frames) - len(part_frames) + 1)
            )
            if contiguous:
                print(f"  video decoded frames: {len(part_frames)} frame(s), identical to a contiguous "
                      f"source slice")
            else:
                print(f"  video decoded frames: {len(part_frames)} frame(s) are NOT a contiguous slice of the "
                      f"source's {len(source_frames)}")
                ok = False

    print("RESULT:", "EXACT" if ok else "MISMATCH")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# merged
# ---------------------------------------------------------------------------
def command_merged(merged: str, inputs: list[str]) -> int:
    ok = True
    merged_packets = packets_of(merged)
    merged_info = stream_info(merged)
    input_packets = [packets_of(path) for path in inputs]
    input_info = [stream_info(path) for path in inputs]

    if list(merged_info.keys()) != list(input_info[0].keys()):
        print(f"  stream set mismatch: merged={sorted(merged_info)} first input={sorted(input_info[0])}")
        ok = False

    size = os.path.getsize(merged)
    print(f"  merged file: {size} bytes, {len(merged_info)} stream(s)")

    for index in sorted(merged_packets):
        expected_total = sum(len(per.get(index, [])) for per in input_packets)
        got = merged_packets[index]
        codec = merged_info.get(index, {}).get("codec_name", "?")
        if len(got) != expected_total:
            print(f"  stream {index} ({codec}): packet count {len(got)} != sum of inputs {expected_total}")
            ok = False
            continue

        position = 0
        mismatches = 0
        junction_packets = 0
        junction_extra_bytes = 0
        for number, per in enumerate(input_packets):
            source_stream = per.get(index, [])
            for offset, expected in enumerate(source_stream):
                if position >= len(got):
                    mismatches += 1
                    break
                actual = got[position]
                position += 1
                if expected["size"] != actual["size"]:
                    first_of_input = offset == 0
                    if first_of_input and expected.get("pos") is not None and actual.get("pos") is not None:
                        expected_bytes = read_bytes(inputs[number], expected["pos"], expected["size"])
                        actual_bytes = read_bytes(merged, actual["pos"], actual["size"])
                        if differs_only_by_parameter_sets(actual_bytes, expected_bytes):
                            junction_packets += 1
                            junction_extra_bytes += len(actual_bytes) - len(expected_bytes)
                            continue
                    mismatches += 1
                    continue
                if expected.get("pos") is None or actual.get("pos") is None:
                    continue
                if read_bytes(inputs[number], expected["pos"], expected["size"]) != \
                        read_bytes(merged, actual["pos"], actual["size"]):
                    mismatches += 1
        print(f"  stream {index} ({codec}): {len(got)} packet(s) (sum of inputs {expected_total}), "
              f"byte mismatches={mismatches}, junction packets with in-band parameter sets={junction_packets} "
              f"(+{junction_extra_bytes} redundant bytes)")
        ok = ok and mismatches == 0

    if any(s.get("codec_type") == "video" for s in merged_info.values()):
        expected_hashes: list[str] = []
        for path in inputs:
            expected_hashes.extend(framemd5(path))
        got_hashes = framemd5(merged)
        if got_hashes == expected_hashes:
            print(f"  video: {len(got_hashes)} decoded frame hash(es) equal the inputs' in order")
        else:
            print(f"  video: decoded frame hashes differ (merged {len(got_hashes)}, "
                  f"inputs {len(expected_hashes)})")
            ok = False

    print("RESULT:", "EXACT" if ok else "MISMATCH")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# limit / time
# ---------------------------------------------------------------------------
def command_limit(path: str, max_bytes: str) -> int:
    size = os.path.getsize(path)
    limit = int(max_bytes)
    ok = size <= limit
    print(f"  {path}: {size} bytes <= {limit} bytes: {ok}")
    return 0 if ok else 1


def command_time(path: str, max_seconds: str) -> int:
    data = probe(path, "format=duration")
    duration = float(data["format"]["duration"])
    limit = float(max_seconds)
    ok = duration <= limit + TIME_TOLERANCE_S
    print(f"  {path}: {duration:.6f} s <= {limit:.6f} s: {ok}")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    if len(argv) < 1:
        print(__doc__, file=sys.stderr)
        return 2
    command = argv[0]
    if command == "part" and len(argv) >= 5:
        return command_part(argv[1], argv[2], argv[3], argv[4])
    if command == "merged" and len(argv) >= 3:
        return command_merged(argv[1], argv[2:])
    if command == "limit" and len(argv) >= 3:
        return command_limit(argv[1], argv[2])
    if command == "time" and len(argv) >= 3:
        return command_time(argv[1], argv[2])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
