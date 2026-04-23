#!/usr/bin/env python3
"""Sweep all 8 decoder flag combinations in local-loop + CD format,
watching for locked=1. The 'f' key cycles flags 0..7."""
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
ser.write(b"t"); time.sleep(0.6)
ser.write(b"F"); time.sleep(0.4)   # → CD
ser.write(b"L"); time.sleep(2.0)   # → local-loop ON, settle

# After begin(), flags=0x2 (SWITCH_PRIMARY). Press 'f' advances to 0x3.
# Cycle all 8; dwell 3s each.
for i in range(8):
    print(f"----- flag cycle #{i+1} -----", flush=True)
    ser.write(b"f")
    time.sleep(3.0)

ser.write(b"L"); time.sleep(0.5)
ser.write(b"F"); time.sleep(0.3)
ser.write(b"t"); time.sleep(0.3)
stop.set(); th.join(1.0); ser.close()
