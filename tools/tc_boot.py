#!/usr/bin/env python3
"""Pulse DTR to reset the ESP32, capture boot log."""
import sys, time
try:
    import serial
except ImportError:
    sys.stderr.write("run via ~/.platformio/penv/bin/python\n"); sys.exit(2)

ser = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.05)
# Pulse RTS to reset into normal (not download) boot. IO0 stays pulled
# up by DTR=0, so the bootloader picks the factory image.
ser.setDTR(False)
ser.setRTS(True)   # EN low → chip in reset
time.sleep(0.1)
ser.setRTS(False)  # release reset → normal boot

t0 = time.monotonic()
while time.monotonic() - t0 < 6.0:
    b = ser.read(512)
    if b:
        sys.stdout.write(b.decode('utf-8', 'replace'))
        sys.stdout.flush()
ser.close()
