#!/usr/bin/env python3
"""Run the tc_* self-asserting test scripts and summarize results.

Each test owns its own serial session (resets the board on entry via
DTR/RTS) so execution order doesn't matter and a failed test doesn't
leave the firmware in a state that poisons the next one. We just run
them sequentially — SERIAL TESTS CAN'T RUN IN PARALLEL; they share
/dev/ttyUSB0.

Exit codes: 0 = all PASS, 1 = any FAIL/ERROR.

Default manifest = the 4 phase-verification tests plus the flag scan.
`tc_probe.py` scenarios, `tc_sanity.py`, `tc_boot.py`, and
`make_beat.py` are exploration / utilities, not regression tests.

Usage:
    ~/.platformio/penv/bin/python tools/run_tests.py
    ~/.platformio/penv/bin/python tools/run_tests.py tc_localloop tc_seek
"""
import os
import subprocess
import sys
import time
from pathlib import Path

TESTS = [
    "tc_localloop",   # Phase 1 — file-fed decoder, locked@1.0 on CD
    "tc_duration",    # Phase 3 — WAV + MP3 duration parse
    "tc_seek",        # Phase 4 — seekToMs hop-to-midpoint
    "tc_phase6",      # Phase 6 — ABS + PROP + needle-up
    "tc_flags_scan",  # decoder flag regression
]

HERE = Path(__file__).resolve().parent


def run_one(name):
    script = HERE / f"{name}.py"
    if not script.exists():
        return name, "MISSING", 0.0
    t0 = time.monotonic()
    # Inherit stdout/stderr so progress is visible live; capture exit code only.
    r = subprocess.run(
        [sys.executable, str(script)],
        cwd=str(HERE),        # so `from _tchelp import …` resolves
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )
    dt = time.monotonic() - t0
    return name, "PASS" if r.returncode == 0 else f"FAIL({r.returncode})", dt


def main():
    picked = sys.argv[1:] if len(sys.argv) > 1 else TESTS
    unknown = [p for p in picked if p not in TESTS]
    if unknown:
        print(f"unknown test(s): {unknown} (known: {TESTS})", file=sys.stderr)
        sys.exit(2)

    results = []
    for name in picked:
        print(f"\n========================================")
        print(f"  RUN: {name}")
        print(f"========================================")
        results.append(run_one(name))

    print("\n========================================")
    print("  SUMMARY")
    print("========================================")
    any_fail = False
    for name, status, dt in results:
        mark = "✓" if status == "PASS" else "✗"
        print(f"  {mark} {name:20s} {status:10s} {dt:6.1f}s")
        if status != "PASS":
            any_fail = True
    print(f"  {len(results)} test(s), "
          f"{sum(1 for _, s, _ in results if s == 'PASS')} passed, "
          f"{sum(1 for _, s, _ in results if s != 'PASS')} failed")
    sys.exit(1 if any_fail else 0)


if __name__ == "__main__":
    main()
