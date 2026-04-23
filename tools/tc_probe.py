#!/usr/bin/env python3
"""Drive the ESP32 firmware over serial to run timecode/pitch probes.

Usage:
    tc_probe.py <scenario>

Opens /dev/ttyUSB0 @ 115200, sends a scripted sequence of keystrokes,
and prints everything it reads with relative-timestamped lines.
Runs from the pio venv (needs pyserial).
"""
import sys, time, threading, select

try:
    import serial
except ImportError:
    sys.stderr.write("run via ~/.platformio/penv/bin/python\n")
    sys.exit(2)

PORT  = "/dev/ttyUSB0"
BAUD  = 115200

class State:
    tc_enabled = None  # None until first known state observed

def reader(ser, stop, t0):
    buf = b""
    while not stop.is_set():
        try:
            data = ser.read(512)
        except Exception as e:
            print(f"[read err] {e}")
            return
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            ts = time.monotonic() - t0
            try:
                s = line.decode('utf-8', 'replace').rstrip()
            except Exception:
                s = "<decode err>"
            print(f"{ts:6.2f}  {s}")
            sys.stdout.flush()
            if "timecode_in: ON" in s:  State.tc_enabled = True
            if "timecode_in: OFF" in s: State.tc_enabled = False

def ensure_tc_on(ser, attempts=3):
    """Toggle 't' until we observe state=ON."""
    for _ in range(attempts):
        if State.tc_enabled is True:
            return
        State.tc_enabled = None
        send(ser, "t", "toggle toward ON")
        # wait up to 1.5s for the response
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline and State.tc_enabled is None:
            time.sleep(0.05)
    if State.tc_enabled is not True:
        print("WARN: could not confirm tc ON", flush=True)

def send(ser, s, desc=""):
    ts = time.monotonic() - send.t0
    print(f"{ts:6.2f}  >>> {repr(s)}  ({desc})")
    sys.stdout.flush()
    ser.write(s.encode())
    ser.flush()

def scenario_sine(ser):
    """Play sine.wav (constant amplitude) and exercise volume, pause, PGA.

    Clean signal → peak variance reflects coupling path, not program
    material. Three sub-probes:
      A. Volume 100→0 in 20% steps: confirm analog output coupling.
      B. Pause/unpause: drops DAC digital output; does rx follow?
      C. PGA (input gain) 100→0: scales signal AND coupling equally
         if coupling is at LINE IN pins; only signal if coupling is
         post-PGA (digital).
    """
    # Force tc ON regardless of starting state.
    ensure_tc_on(ser)
    time.sleep(1.5)
    send(ser, "V", "nudge vol to confirm starting level")
    time.sleep(0.3)
    send(ser, "v", "back to 100")
    time.sleep(0.3)

    # Navigate to house.wav (long file, last entry, index 5) — sine.wav
    # was only ~8s long and hit EOF mid-probe.
    for _ in range(10):
        send(ser, "w", "top"); time.sleep(0.15)
    time.sleep(0.5)
    for i in range(5):
        send(ser, "s", f"down {i+1}"); time.sleep(0.15)
    time.sleep(0.5)
    send(ser, "\r", "play house.wav")
    time.sleep(3.0)

    # A. Volume ramp (don't go to 0 — setVolume(0) breaks the StreamCopy
    # write path and the track stops). Stop at 10.
    print("----- A. VOLUME RAMP -----", flush=True)
    for step in range(9):  # 100 → 10
        send(ser, "v", f"vol -10 (step {step+1})")
        time.sleep(1.5)
    time.sleep(1.0)
    # Back up
    for _ in range(9):
        send(ser, "V", "vol +10"); time.sleep(0.15)
    time.sleep(2.0)  # observe at vol=100 for reference

    # B. Pause test — DAC digital mute
    print("----- B. PAUSE TEST -----", flush=True)
    send(ser, "p", "pause (setMute)")
    time.sleep(3.0)
    send(ser, "p", "unpause")
    time.sleep(2.0)

    # C. PGA ramp
    print("----- C. PGA RAMP -----", flush=True)
    # g reduces input gain by 10/100. 10 presses → 0.
    for step in range(10):
        send(ser, "g", f"gain -10 (step {step+1})")
        time.sleep(1.2)
    time.sleep(2.0)
    # Restore
    for _ in range(10):
        send(ser, "G", "gain +10"); time.sleep(0.08)

    send(ser, "b", "stop")
    time.sleep(1.5)
    send(ser, "t", "tc OFF")


def scenario_turntable(ser):
    """Keylock-mode pitch tracking test.

    Keylock uses WSOLA at the native sample rate — I²S SR stays fixed
    regardless of player speed — so the ADC reference doesn't drift
    with DAC speed, breaking the feedback loop that Pitched mode has
    on this codec.

    Also drop PGA first — Dan's turntable signal is hotter than last
    bring-up (rx railed at 32767 with +24dB PGA even before playback).
    """
    ensure_tc_on(ser)
    time.sleep(1.5)

    # Drop PGA so the turntable signal doesn't rail. 5 taps → +12dB.
    for i in range(5):
        send(ser, "g", f"PGA -3dB (tap {i+1})"); time.sleep(0.2)
    time.sleep(1.0)
    print("----- PGA reduced — observe 4s of turntable-only signal -----", flush=True)
    time.sleep(4.0)

    # Switch to keylock mode. Toggles mode; only takes effect on next play.
    send(ser, "K", "enable keylock (WSOLA)")
    time.sleep(0.8)

    # Navigate to house.wav (index 5).
    for _ in range(10):
        send(ser, "w", "top"); time.sleep(0.15)
    time.sleep(0.5)
    for i in range(5):
        send(ser, "s", f"down {i+1}"); time.sleep(0.15)
    time.sleep(0.5)
    send(ser, "\r", "play house.wav (keylock)")
    time.sleep(4.0)

    print("----- SPIN PLATTER at 33⅓ NOW -----", flush=True)
    time.sleep(10.0)
    print("----- KEEP SPINNING -----", flush=True)
    time.sleep(8.0)
    print("----- TRY NUDGING SLOWER / FASTER BY HAND -----", flush=True)
    time.sleep(10.0)
    print("----- HOLD PLATTER STILL (but keep stylus on record) -----", flush=True)
    time.sleep(6.0)

    send(ser, "b", "stop")
    time.sleep(1.0)
    send(ser, "K", "disable keylock")  # restore for next run
    time.sleep(0.5)


def scenario_smoothing(ser):
    """Self-loop with keylock to characterize decoder jitter vs EMA-
    smoothed player speed. Platter-independent (use this for calibration
    when the turntable isn't stable).

    In keylock mode WSOLA preserves pitch, so the DAC-driven carrier
    stays at 1 kHz regardless of player speed — the decoder always
    reports ~1.0×, which is exactly the steady-state we want to measure
    smoothing on.
    """
    ensure_tc_on(ser)
    time.sleep(1.5)
    # Boost PGA for the self-loop only. The default PGA=0 makes sense
    # for a line-level turntable cartridge but leaves the coupled
    # self-loop signal down in the noise. ~18 dB (75/100) restores a
    # decodable rx on the coupling path.
    for i in range(8):
        send(ser, "G", f"PGA +3dB (tap {i+1})"); time.sleep(0.15)
    time.sleep(0.8)
    send(ser, "K", "enable keylock")
    time.sleep(0.5)

    # timecode.wav = index 2.
    for _ in range(10):
        send(ser, "w", "top"); time.sleep(0.15)
    time.sleep(0.3)
    for i in range(2):
        send(ser, "s", f"down {i+1}"); time.sleep(0.15)
    time.sleep(0.3)
    send(ser, "\r", "play timecode.wav")
    time.sleep(4.0)

    print("----- OBSERVE 20s — compare [tc] speed (raw) vs [play] speed (smoothed) -----", flush=True)
    time.sleep(20.0)

    send(ser, "b", "stop")
    time.sleep(0.5)
    send(ser, "K", "disable keylock")


def scenario_selfloop(ser):
    """Play timecode.wav → DAC→ADC coupling → decoder. Closed self-test.

    With both formats at 1kHz carrier, ZC-based speed estimation should
    land near 1.0x even though bit-lock won't latch (vinyl decoder, CD
    signal). Then stress it: bump speed to 1.2x / 0.8x via [ and ] keys,
    see if the decoder tracks the DAC clock change.
    """
    ensure_tc_on(ser)
    time.sleep(1.5)

    # timecode.wav is index 2 (house.mp3=0, amen-16=1, timecode=2,
    # beat=3, sine=4, house.wav=5).
    for _ in range(10):
        send(ser, "w", "top"); time.sleep(0.15)
    time.sleep(0.5)
    for i in range(2):
        send(ser, "s", f"down {i+1}"); time.sleep(0.15)
    time.sleep(0.5)
    send(ser, "\r", "play timecode.wav")
    time.sleep(5.0)

    print("----- OBSERVE 20s @ 1.0x -----", flush=True)
    time.sleep(20.0)

    send(ser, "b", "stop")
    time.sleep(1.0)


def scenario_outputs(ser):
    """A/B the ES8388 output drivers to find the dominant coupling path.

    Sequence: arm tc, play a long track, then cycle runtime keys
        3 → default (LOUT1+ROUT1+LOUT2+ROUT2, reg 0x04=0x3C)
        1 → HP only (LOUT1/ROUT1, reg 0x04=0x0C)
        2 → LINE OUT only (LOUT2/ROUT2, reg 0x04=0x30) — HP should go silent
        M → DAC volume -96dB (full digital mute) — rx MUST drop if DAC is the source
        U → DAC 0dB (unmute)
        3 → back to default
    Dwell 5s at each state so rx peaks over multiple [tc] windows speak for themselves.
    """
    ensure_tc_on(ser)
    time.sleep(1.5)

    # Navigate to house.wav (index 5).
    for _ in range(10):
        send(ser, "w", "top"); time.sleep(0.15)
    time.sleep(0.5)
    for i in range(5):
        send(ser, "s", f"down {i+1}"); time.sleep(0.15)
    time.sleep(0.5)
    send(ser, "\r", "play house.wav")
    time.sleep(5.0)

    def dwell(label, key, secs=5.0):
        print(f"----- {label} -----", flush=True)
        send(ser, key, label)
        time.sleep(secs)

    dwell("A. DEFAULT (all outputs)",  "3", 6.0)
    dwell("B. HP ONLY (LOUT2/ROUT2 disabled)",   "1", 6.0)
    dwell("C. LINE OUT ONLY (LOUT1/ROUT1 off)",  "2", 6.0)
    dwell("D. BACK TO DEFAULT",                  "3", 3.0)
    dwell("E. DAC MUTED (digital -96dB)",        "M", 6.0)
    dwell("F. DAC UNMUTED",                      "U", 4.0)
    dwell("G. FINAL CHECK (default)",            "3", 3.0)

    send(ser, "b", "stop")
    time.sleep(1.0)


def scenario_volume(ser):
    # Make sure we're at Browser and tc is off for a clean start.
    send(ser, "b", "force Browser (no-op if already)")
    time.sleep(0.5)
    send(ser, "t", "toggle tc (state unknown — we'll check)")
    time.sleep(2.5)
    send(ser, "?", "print help so we can see current state")
    time.sleep(1.0)
    # We need timecode ON — if the toggle turned it off, toggle back.
    # Caller should eyeball the log; cleaner path is a dedicated 'tc on' key.
    # For now: toggle once more if needed via an interactive step.
    # Actually: easier to just force ON via toggling until we see a [tc] line.
    # The reader thread prints — we can't race condition detect here. Just
    # assume t was OFF initially (Dan's last monitor snippet ended with "OFF").
    # If not, user will see and re-run.
    print("----- VOLUME PROBE -----", flush=True)
    # Navigate: we don't know current selection. Press 's' enough times to
    # move down to a long track. Browser scan count was 6 last time;
    # house.wav was index 5. We can 'w' 10x to guarantee top, then 's' to
    # target.
    for _ in range(10):
        send(ser, "w", "up (force to top)")
        time.sleep(0.08)
    time.sleep(0.3)
    # Target index 5 = house.wav (from Dan's previous listing: house.mp3,
    # amen-16.wav, timecode.wav, beat.wav, sine.wav, house.wav).
    for i in range(5):
        send(ser, "s", f"down {i+1}")
        time.sleep(0.08)
    time.sleep(0.3)
    send(ser, "\r", "Enter → play house.wav")
    time.sleep(4.0)  # let it play, expect high rx peaks if bug reproduces
    # Now the volume taps.
    for i in range(10):
        send(ser, "v", f"vol down #{i+1}")
        time.sleep(1.2)  # enough for a [tc] line at 1Hz
    send(ser, "b", "stop")
    time.sleep(2.0)
    for _ in range(10):
        send(ser, "V", "vol back up")
        time.sleep(0.1)

def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    stop = threading.Event()
    t0 = time.monotonic()
    send.t0 = t0
    th = threading.Thread(target=reader, args=(ser, stop, t0), daemon=True)
    th.start()
    # give the reader a moment to flush any pending output
    time.sleep(0.8)
    try:
        scenario = sys.argv[1] if len(sys.argv) > 1 else "sine"
        if scenario == "sine":
            scenario_sine(ser)
        elif scenario == "selfloop":
            scenario_selfloop(ser)
        elif scenario == "smoothing":
            scenario_smoothing(ser)
        elif scenario == "turntable":
            scenario_turntable(ser)
        elif scenario == "outputs":
            scenario_outputs(ser)
        elif scenario == "volume":
            scenario_volume(ser)
        else:
            print(f"unknown scenario: {scenario}", file=sys.stderr)
            sys.exit(2)
    finally:
        time.sleep(0.8)
        stop.set()
        th.join(timeout=2.0)
        ser.close()

if __name__ == "__main__":
    main()
