#!/usr/bin/env python3
"""Phase 3 test: track duration parsed from WAV and MP3 headers.

Plays house.wav, confirms the parser identifies SR/channels/bits and
a non-zero duration. Repeats for house.mp3 (CBR estimate via first
frame bitrate). Also checks positionMs increments roughly 1:1 with
wall time during playback.
"""
import re
import time
from _tchelp import Probe, run_test, require_line

WAV_LINE = re.compile(
    r"\[play\] WAV: (?P<sr>\d+) Hz, (?P<ch>\d+) ch, (?P<bits>\d+)-bit, "
    r"data@(?P<ds>\d+), (?P<dur>\d+) ms"
)
MP3_LINE = re.compile(
    r"\[play\] MP3: (?P<br>\d+) kbps, audio@(?P<ds>\d+), ~(?P<dur>\d+) ms"
)
POS_LINE = re.compile(r"\[play\] pos=\d+\s+ms=(?P<cur>\d+)/(?P<dur>\d+)")


def find_match(lines, pat):
    for _ts, s in lines:
        m = pat.search(s)
        if m:
            return m
    return None


def positions_over_time(lines):
    return [(ts, int(POS_LINE.search(s).group("cur")))
            for ts, s in lines if POS_LINE.search(s)]


def test(p: Probe):
    # Navigate to top, then play house.wav (index 5: last audio file).
    for _ in range(10):
        p.send(b"w", "top"); time.sleep(0.1)
    time.sleep(0.3)
    for i in range(5):
        p.send(b"s", f"down {i+1}"); time.sleep(0.1)
    time.sleep(0.3)

    p.marker("PLAY house.wav")
    wav_start_idx = len(p.lines)
    p.send(b"\r", "play"); time.sleep(5.0)
    p.send(b"b", "stop"); time.sleep(0.8)

    # Then house.mp3 (index 0).
    for _ in range(10):
        p.send(b"w", "top"); time.sleep(0.1)
    time.sleep(0.3)
    p.marker("PLAY house.mp3")
    mp3_start_idx = len(p.lines)
    p.send(b"\r", "play"); time.sleep(5.0)
    p.send(b"b", "stop"); time.sleep(0.5)

    # ---- WAV assertions ----
    wav_lines = p.lines[wav_start_idx:mp3_start_idx]
    m = find_match(wav_lines, WAV_LINE)
    assert m, (
        "no [play] WAV: … line captured; duration parser failed to fire or "
        "house.wav isn't recognized as RIFF/WAVE"
    )
    sr   = int(m.group("sr"));  assert sr in (44100, 48000), f"WAV SR unexpected: {sr}"
    ch   = int(m.group("ch"));  assert ch in (1, 2),         f"WAV channels unexpected: {ch}"
    bits = int(m.group("bits")); assert bits in (16, 24, 32), f"WAV bits unexpected: {bits}"
    wav_dur = int(m.group("dur"))
    assert 5_000 <= wav_dur <= 30 * 60 * 1000, f"WAV duration wildly off: {wav_dur} ms"

    # positionMs should climb roughly 1:1 with wall time during WAV playback.
    positions = positions_over_time(wav_lines)
    assert len(positions) >= 3, f"too few [play] pos lines during WAV ({len(positions)})"
    # First-to-last span across ~4 s of playback at 1× → should climb ~3000–5000 ms.
    span = positions[-1][1] - positions[0][1]
    assert 3_000 <= span <= 5_500, (
        f"WAV positionMs climbed {span} ms across the playback window; "
        f"expected ~3000–5000 ms at 1× speed"
    )

    # ---- MP3 assertions ----
    mp3_lines = p.lines[mp3_start_idx:]
    m = find_match(mp3_lines, MP3_LINE)
    assert m, (
        "no [play] MP3: … line captured; first-frame bitrate parse failed"
    )
    br = int(m.group("br"))
    assert 32 <= br <= 320, f"MP3 bitrate unexpected: {br} kbps"
    mp3_dur = int(m.group("dur"))
    assert 5_000 <= mp3_dur <= 30 * 60 * 1000, f"MP3 duration wildly off: {mp3_dur} ms"

    positions = positions_over_time(mp3_lines)
    assert len(positions) >= 3, f"too few [play] pos lines during MP3 ({len(positions)})"
    # MP3 positionMs leads actual playback by ~100-150 ms due to decoder buffering,
    # so the span is a little looser than WAV.
    span = positions[-1][1] - positions[0][1]
    assert 3_000 <= span <= 6_000, (
        f"MP3 positionMs climbed {span} ms across the playback window; "
        f"expected ~3000–6000 ms"
    )


run_test("phase3_duration", test)
