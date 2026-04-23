#!/usr/bin/env python3
"""Phase 4 verification: play a track, press S to jump to midpoint,
confirm positionMs jumps."""
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

def send(b, desc):
    print(f"{time.monotonic()-t0:5.2f}  >>> {b!r}  ({desc})", flush=True)
    ser.write(b)

th = threading.Thread(target=rd, daemon=True); th.start()
time.sleep(1.0)

# Navigate to house.wav.
for _ in range(10):
    send(b"w", "top"); time.sleep(0.12)
time.sleep(0.3)
for i in range(5):
    send(b"s", f"down {i+1}"); time.sleep(0.12)
time.sleep(0.3)
print("----- PLAY house.wav -----", flush=True)
send(b"\r", "play"); time.sleep(3.0)
print("----- SEEK to midpoint -----", flush=True)
send(b"S", "seekToMs midpoint"); time.sleep(4.0)
send(b"b", "stop"); time.sleep(0.5)

stop.set(); th.join(1.0); ser.close()
