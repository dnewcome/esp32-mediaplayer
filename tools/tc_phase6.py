#!/usr/bin/env python3
"""Phase 6 end-to-end test: local-loop drives decoder position, plays
a track, verifies position seek and past-end-stop behavior.

timecode.wav is read from its start when local-loop is toggled ON,
so the decoder position walks 0 → ... slowly. The player's track
should be seeked to match. If the track is SHORTER than 950 sec of
timecode, it'll hit past-end and stop (absolute mode).

In Proportional mode, the track would be scrubbed across its full
duration as tc walks the ~15 min of CD timecode.
"""
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

# Put decoder in CD local-loop mode so position increments from 0.
send(b"t", "arm tc");      time.sleep(0.5)
send(b"F", "→ CD format"); time.sleep(0.5)
send(b"K", "→ keylock");   time.sleep(0.5)

# Navigate to house.wav (index 5) and play.
for _ in range(10):
    send(b"w", "top"); time.sleep(0.1)
time.sleep(0.3)
for i in range(5):
    send(b"s", f"down {i+1}"); time.sleep(0.1)
time.sleep(0.3)
print("----- PLAY house.wav (before local-loop so tc drives it) -----", flush=True)
send(b"\r", "play"); time.sleep(3.0)

# Start local-loop: decoder position will begin walking from 0.
print("----- ENABLE LOCAL-LOOP (tc position starts walking) -----", flush=True)
send(b"L", "local-loop ON"); time.sleep(10.0)

print("----- SWITCH TO PROPORTIONAL MODE -----", flush=True)
send(b"P", "→ proportional"); time.sleep(8.0)

print("----- DISABLE LOCAL-LOOP (needle up — expect pause) -----", flush=True)
send(b"L", "local-loop OFF"); time.sleep(4.0)

send(b"b", "stop"); time.sleep(0.5)
send(b"K", "→ pitched"); time.sleep(0.3)
send(b"F", "→ vinyl format"); time.sleep(0.3)
send(b"t", "tc off"); time.sleep(0.3)

stop.set(); th.join(1.0); ser.close()
