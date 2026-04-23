#!/usr/bin/env python3
"""Phase 1 verification: arm tc, enable local-loop, read 15s of traces."""
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
ser.write(b"t"); time.sleep(1.0)     # arm decoder
ser.write(b"F"); time.sleep(0.5)     # switch decoder to CD format
ser.write(b"L"); time.sleep(18.0)    # enable local-loop; expect locked=1 within ~300ms
ser.write(b"L"); time.sleep(1.0)
ser.write(b"F"); time.sleep(0.3)     # restore default vinyl format
ser.write(b"t"); time.sleep(0.5)
stop.set(); th.join(1.0); ser.close()
