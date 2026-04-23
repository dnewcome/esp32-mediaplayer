#!/usr/bin/env python3
"""Phase 6 test: position-driven seek, PROP speed scaling, needle-up pause.

All three behaviors in one scenario, driven by local-loop so no
hardware signal is needed:

  1. ABS mode: enable local-loop while a track is playing. The first
     [tc] seek drift=… (mode=ABS) line must fire within ~1 s.
  2. PROP mode: press P. Player speed should drop to match
     track_dur/vinyl_len (house.wav on CD ≈ 0.14×). Seeks should be
     rare, not every 300 ms.
  3. Needle-up: toggle local-loop OFF. Within ~500 ms, the firmware
     should log `unlock >400 ms → pause (needle up)`.
"""
import re
import time
from _tchelp import Probe, run_test, require_line

SEEK_LINE  = re.compile(r"\[tc\] seek drift=(?P<drift>\d+) → (?P<target>\d+) ms \(mode=(?P<mode>\w+)\)")
PLAY_LINE  = re.compile(r"\[play\] pos=\d+\s+ms=\d+/\d+\s+speed=(?P<speed>-?\d+\.\d+)x")


def test(p: Probe):
    p.send(b"t", "arm tc");       time.sleep(0.5)
    p.send(b"F", "→ CD format");  time.sleep(0.5)
    p.send(b"K", "→ keylock");    time.sleep(0.5)

    # Navigate to house.wav (index 5) and play.
    for _ in range(10):
        p.send(b"w", "top"); time.sleep(0.1)
    time.sleep(0.3)
    for i in range(5):
        p.send(b"s", f"down {i+1}"); time.sleep(0.1)
    time.sleep(0.3)
    p.marker("PLAY house.wav (keylock, pre-local-loop)")
    p.send(b"\r", "play"); time.sleep(3.0)

    # Phase 1 in the scenario: local-loop ON → ABS seek should fire.
    p.marker("LOCAL-LOOP ON — expect ABS seek within ~1 s")
    abs_start = len(p.lines)
    p.send(b"L", "local-loop ON"); time.sleep(10.0)

    # Phase 2: switch to proportional.
    p.marker("PROP MODE")
    prop_start = len(p.lines)
    p.send(b"P", "→ proportional"); time.sleep(8.0)

    # Phase 3: local-loop OFF → needle-up pause within ~400 ms.
    p.marker("LOCAL-LOOP OFF — expect needle-up pause")
    pause_start = len(p.lines)
    p.send(b"L", "local-loop OFF"); time.sleep(2.0)

    p.send(b"b", "stop"); time.sleep(0.3)

    # ---- assertions ----

    # ABS seek
    abs_seeks = [m for _ts, s in p.lines[abs_start:prop_start]
                 for m in [SEEK_LINE.search(s)] if m and m.group("mode") == "ABS"]
    assert len(abs_seeks) >= 1, (
        "expected at least one `[tc] seek … (mode=ABS)` line after local-loop ON; "
        "position-driven seek never fired"
    )

    # PROP: speed should drop to ~0.1–0.2× (house.wav 135s on CD 950s ≈ 0.14).
    prop_speeds = []
    for _ts, s in p.lines[prop_start:pause_start]:
        m = PLAY_LINE.search(s)
        if m:
            prop_speeds.append(float(m.group("speed")))
    assert prop_speeds, "no [play] speed= lines captured during PROP dwell"
    # Drop the first one (may still reflect ABS-mode speed as it transitions).
    stable_prop = prop_speeds[1:] if len(prop_speeds) > 1 else prop_speeds
    assert all(0.10 < v < 0.25 for v in stable_prop), (
        f"expected all PROP-mode speeds in (0.10, 0.25), got {stable_prop}; "
        f"transportScale multiplier isn't being applied"
    )

    # PROP seeks should be sparse (≤5 across 8 s), not every 300 ms (≥20).
    prop_seek_count = sum(
        1 for _ts, s in p.lines[prop_start:pause_start]
        if (m := SEEK_LINE.search(s)) and m.group("mode") == "PROP"
    )
    assert prop_seek_count <= 5, (
        f"PROP-mode seeked {prop_seek_count} times in 8 s — "
        f"the speed-scale fix for continuous-drift seek-thrashing regressed"
    )

    # Needle-up pause
    pause_lines = p.lines[pause_start:]
    require_line(pause_lines, "unlock >400 ms → pause (needle up)",
                 "needle-up hysteresis didn't fire after local-loop OFF")


run_test("phase6_integration", test)
