#!/usr/bin/env python3
"""Phase 4 test: seekToMs primitive.

Plays house.wav, waits ~3 s (player at ~3 s in), presses S which
should seek to the track midpoint. Checks that:
  - `seekToMs(<midpoint>) → OK` is logged
  - next [play] positionMs line lands near the midpoint (±200 ms)
  - subsequent position lines continue to climb from there
"""
import re
import time
from _tchelp import Probe, run_test, require_line

SEEK_LINE = re.compile(r"seekToMs\((?P<target>\d+)\) → OK")
POS_LINE  = re.compile(r"\[play\] pos=\d+\s+ms=(?P<cur>\d+)/(?P<dur>\d+)")


def test(p: Probe):
    # Navigate to house.wav (index 5).
    for _ in range(10):
        p.send(b"w", "top"); time.sleep(0.1)
    time.sleep(0.3)
    for i in range(5):
        p.send(b"s", f"down {i+1}"); time.sleep(0.1)
    time.sleep(0.3)

    p.marker("PLAY house.wav")
    p.send(b"\r", "play"); time.sleep(3.0)

    p.marker("SEEK to midpoint")
    seek_at_idx = len(p.lines)
    p.send(b"S", "seekToMs midpoint"); time.sleep(4.0)
    p.send(b"b", "stop"); time.sleep(0.3)

    post_seek = p.lines[seek_at_idx:]

    # Seek acknowledgement with a target.
    target_ms = None
    for _ts, s in post_seek:
        m = SEEK_LINE.search(s)
        if m:
            target_ms = int(m.group("target"))
            break
    assert target_ms is not None, "seekToMs(...) → OK not seen after S keypress"
    assert target_ms > 10_000, f"target {target_ms} ms implausibly small for house.wav midpoint"

    # First [play] pos after seek should land near target.
    # WSOLA has latency; allow ±200 ms slack.
    post_seek_positions = []
    for _ts, s in post_seek:
        m = POS_LINE.search(s)
        if m:
            post_seek_positions.append(int(m.group("cur")))
    assert len(post_seek_positions) >= 2, (
        f"need ≥2 positionMs lines after seek, got {len(post_seek_positions)}"
    )
    first_after = post_seek_positions[0]
    err = abs(first_after - target_ms)
    assert err <= 200, (
        f"first positionMs after seek = {first_after}, target = {target_ms}, "
        f"delta {err} ms — seek didn't land near target"
    )

    # Position must continue to climb at ~1× after the seek (not bouncing
    # back to pre-seek).
    climb = post_seek_positions[-1] - post_seek_positions[0]
    assert climb > 1_500, (
        f"positionMs climbed only {climb} ms after seek — playback may be stalled"
    )


run_test("phase4_seek", test)
