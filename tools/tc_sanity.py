#!/usr/bin/env python3
"""Quick boot sanity check: open port, arm tc, log for 6s."""
import sys, time, threading
try:
    import serial
except ImportError:
    sys.stderr.write("run via ~/.platformio/penv/bin/python\n"); sys.exit(2)

ser = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.1)
t0 = time.monotonic()
stop = threading.Event()

def rd():
    buf = b""
    while not stop.is_set():
        d = ser.read(512)
        if not d: continue
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print(f"{time.monotonic()-t0:5.2f}  {line.decode('utf-8', 'replace').rstrip()}")
            sys.stdout.flush()

th = threading.Thread(target=rd, daemon=True); th.start()
time.sleep(1.0)
ser.write(b"t"); time.sleep(6.0)
stop.set(); th.join(1.0); ser.close()
