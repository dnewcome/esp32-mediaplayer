#!/usr/bin/env python3
"""Flag-sweep regression test: with local-loop on CD format, at least
one of the 8 decoder flag combinations must achieve `locked=1`.

Baseline expectation: flags 0x0, 0x3, 0x4, 0x7 all produce lock (even
parity); 0x1, 0x2, 0x5, 0x6 do not. Mostly a smoke check that the
decoder still responds to flag changes and that /timecode.wav hasn't
drifted from CD-compatible format.
"""
import re
import time
from _tchelp import Probe, run_test

TC_LINE = re.compile(r"\[tc\] .*locked=(?P<locked>\d)")
FLAG_LINE = re.compile(r"tc flags=0x(?P<hex>[0-7])")


def test(p: Probe):
    p.send(b"t", "arm tc");        time.sleep(0.5)
    p.send(b"F", "→ CD format");   time.sleep(0.5)
    p.send(b"L", "local-loop ON"); time.sleep(2.0)

    any_lock = False
    per_flag_lock = {}
    for i in range(8):
        p.marker(f"flag cycle #{i+1}")
        start = len(p.lines)
        p.send(b"f", "cycle flags"); time.sleep(3.0)
        # Identify which flag this segment runs on.
        cur_flag = None
        for _ts, s in p.lines[start:]:
            m = FLAG_LINE.search(s)
            if m:
                cur_flag = int(m.group("hex"), 16)
                break
        locked_in_window = sum(
            1 for _ts, s in p.lines[start:]
            if (m := TC_LINE.search(s)) and m.group("locked") == "1"
        )
        if cur_flag is not None:
            per_flag_lock[cur_flag] = locked_in_window
        if locked_in_window >= 2:
            any_lock = True

    p.send(b"L", "local-loop OFF"); time.sleep(0.3)
    p.send(b"F", "→ vinyl format"); time.sleep(0.3)
    p.send(b"t", "tc off"); time.sleep(0.2)

    print(f"[flags_scan] per-flag locked-line count: {per_flag_lock}", flush=True)
    assert any_lock, (
        f"no decoder flag combination achieved locked=1 on /timecode.wav; "
        f"per-flag counts {per_flag_lock}"
    )


run_test("flags_scan", test)
