"""Shared helpers for the tc_*.py test/probe scripts.

Centralizes what used to be duplicated in every script: opening the
serial port, a reader thread that captures every line into a buffer,
a keystroke sender with timestamped logging, and a DTR/RTS-based
board reset so each run starts from a known-good boot state.

Also exposes a small assertion DSL — `require(buf, ...)` helpers that
scan the captured log for expected traces and raise AssertionError
with a clear message on miss. The runner (`run_tests.py`) picks up
exit codes from each script.
"""
import re
import sys
import threading
import time
try:
    import serial
except ImportError:
    sys.stderr.write("run via ~/.platformio/penv/bin/python\n"); sys.exit(2)

PORT = "/dev/ttyUSB0"
BAUD = 115200


class Probe:
    """Context-manager wrapper around a serial port with a reader thread.

    Usage:
        with Probe(reset=True, boot_wait=4.0) as p:
            p.send(b"t", "arm tc")
            time.sleep(1.0)
            # ... drive the board ...
        # After `with` block, p.lines holds the captured log and p.t0
        # is the timeline origin. Run assertions on p.lines.
    """

    def __init__(self, reset=True, boot_wait=4.5, port=PORT, baud=BAUD):
        self.port = port
        self.baud = baud
        self.reset = reset
        self.boot_wait = boot_wait
        self.ser = None
        self.stop = threading.Event()
        self.reader = None
        self.t0 = 0.0
        self.lines = []   # [(relative_ts, line_str), ...]

    def __enter__(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.05)
        if self.reset:
            # EN low (reset) → release; IO0 stays pulled up by DTR=0
            # so the bootloader runs the factory image, not download mode.
            self.ser.setDTR(False)
            self.ser.setRTS(True)
            time.sleep(0.1)
            self.ser.setRTS(False)
        self.t0 = time.monotonic()
        self.reader = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader.start()
        if self.reset:
            # Boot + setup() + LUT build + SD scan take a few seconds.
            time.sleep(self.boot_wait)
        return self

    def __exit__(self, exc_type, exc, tb):
        # Give the reader a moment to flush any late lines before we stop it.
        time.sleep(0.3)
        self.stop.set()
        if self.reader:
            self.reader.join(timeout=2.0)
        if self.ser:
            self.ser.close()
        return False

    def _reader_loop(self):
        buf = b""
        while not self.stop.is_set():
            try:
                data = self.ser.read(512)
            except Exception as e:
                print(f"[read err] {e}", file=sys.stderr)
                return
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                ts = time.monotonic() - self.t0
                try:
                    s = line.decode("utf-8", "replace").rstrip()
                except Exception:
                    s = "<decode err>"
                self.lines.append((ts, s))
                print(f"{ts:6.2f}  {s}")
                sys.stdout.flush()

    def send(self, b, desc=""):
        ts = time.monotonic() - self.t0
        print(f"{ts:6.2f}  >>> {b!r}  ({desc})")
        sys.stdout.flush()
        self.ser.write(b)

    def marker(self, text):
        """Drop a divider into stdout to separate phases in the log."""
        print(f"----- {text} -----", flush=True)


# ---------- Assertions ----------
#
# Each `require_*` helper scans the captured `Probe.lines` buffer and
# raises AssertionError on miss. All raise with enough context that
# a reader of the CI log can see what was expected and what was there.

def _line_matches(line, pattern_or_literal):
    if hasattr(pattern_or_literal, "search"):
        return pattern_or_literal.search(line) is not None
    return pattern_or_literal in line


def require_line(lines, pattern, msg=""):
    """At least one captured line must match `pattern` (str or re.Pattern)."""
    for _ts, s in lines:
        if _line_matches(s, pattern):
            return s
    raise AssertionError(f"expected line matching {pattern!r}; {msg}")


def require_count(lines, pattern, at_least, msg=""):
    """Count of lines matching `pattern` must be >= at_least."""
    n = sum(1 for _ts, s in lines if _line_matches(s, pattern))
    if n < at_least:
        raise AssertionError(f"expected ≥{at_least} lines matching {pattern!r}, "
                             f"got {n}; {msg}")
    return n


def require_no_line(lines, pattern, msg=""):
    for _ts, s in lines if False else []:
        pass
    for _ts, s in lines:
        if _line_matches(s, pattern):
            raise AssertionError(f"unexpected line matching {pattern!r}: {s!r}; {msg}")


def extract_int(line, pattern):
    """Pull a named group 'n' (int) out of the first match of `pattern` in `line`."""
    m = pattern.search(line) if hasattr(pattern, "search") else re.search(pattern, line)
    if not m:
        return None
    return int(m.group("n"))


def extract_float(line, pattern):
    m = pattern.search(line) if hasattr(pattern, "search") else re.search(pattern, line)
    if not m:
        return None
    return float(m.group("n"))


# ---------- Test-script main pattern ----------

def run_test(test_name, test_fn):
    """Wrap a test_fn(Probe) call: run it, catch AssertionError, exit appropriately.

    Keeps each test script's top-level short:

        from _tchelp import Probe, run_test, require_line
        def test(p):
            p.send(b"t", "arm")
            time.sleep(3)
            require_line(p.lines, "timecode_in: ON")
        run_test("phase1", test)
    """
    print(f"=== {test_name} ===", flush=True)
    try:
        with Probe() as p:
            test_fn(p)
        print(f"[{test_name}] PASS", flush=True)
        sys.exit(0)
    except AssertionError as e:
        print(f"[{test_name}] FAIL: {e}", flush=True)
        sys.exit(1)
    except Exception as e:
        print(f"[{test_name}] ERROR: {e.__class__.__name__}: {e}", flush=True)
        sys.exit(2)
