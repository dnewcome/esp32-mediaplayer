#!/usr/bin/env python3
"""Phase 1 test: local-loop diagnostic mode.

With the board fresh (no turntable attached), arm tc, switch to CD
format, enable local-loop. The decoder should lock on /timecode.wav
within ~1 s, reporting speed≈1.0× steady. This validates the
file-fed signal path without any hardware dependency.
"""
import re
import time
from _tchelp import Probe, run_test, require_line, require_count

TC_LINE = re.compile(r"\[tc\] speed=(?P<speed>-?\d+\.\d+)\s+locked=(?P<locked>\d)")


def test(p: Probe):
    p.send(b"t", "arm tc");         time.sleep(0.5)
    p.send(b"F", "→ CD format");    time.sleep(0.5)
    p.send(b"L", "local-loop ON");  time.sleep(8.0)
    p.send(b"L", "local-loop OFF"); time.sleep(0.5)

    # The firmware must acknowledge each toggle on the serial bus.
    require_line(p.lines, "timecode_in: ON",
                 "tc was never armed — firmware may have missed the 't' byte")
    require_line(p.lines, "tc format: CD",
                 "format toggle to CD not acknowledged")
    require_line(p.lines, "local-loop: /timecode.wav opened",
                 "local-loop file open failed — is /timecode.wav on the SD card?")

    # At least ~5 locked [tc] lines at speed ~1.0 during the dwell window.
    locked_count = 0
    near_one_count = 0
    for _ts, s in p.lines:
        m = TC_LINE.search(s)
        if not m:
            continue
        if m.group("locked") == "1":
            locked_count += 1
        if 0.95 < abs(float(m.group("speed"))) < 1.05 and m.group("locked") == "1":
            near_one_count += 1
    assert locked_count >= 5, (
        f"expected ≥5 [tc] locked=1 lines during the 8 s local-loop window, got {locked_count}; "
        f"decoder may not be matching /timecode.wav (wrong format or flags?)"
    )
    assert near_one_count >= 5, (
        f"expected ≥5 locked lines with speed in [0.95, 1.05] (local-loop plays at 1.0×), "
        f"got {near_one_count}; decoder is locking but reading the wrong speed"
    )


run_test("phase1_localloop", test)
