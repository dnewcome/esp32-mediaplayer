#!/usr/bin/env python3
"""Generate a simple 4/4 test loop for vinyl_demo.

Beats 1+3: kick (low pitched-sine with fast decay).
Beats 2+4: hi-hat (high-passed white noise burst).

  python3 tools/make_beat.py --bpm 120 --duration 120 --out native/media/beat.wav
"""
import argparse
import struct
import wave
import numpy as np

SR = 44100


def kick(length_s: float) -> np.ndarray:
    """Classic 808-ish kick: ~80→40 Hz sweep, exponential amp decay."""
    n = int(length_s * SR)
    t = np.arange(n) / SR
    freq = 80.0 - 40.0 * np.clip(t / 0.08, 0, 1)
    amp  = np.exp(-t / 0.12)
    return 0.9 * amp * np.sin(2 * np.pi * np.cumsum(freq) / SR)


def hat(length_s: float) -> np.ndarray:
    """Hi-hat: white noise, crude HPF via first-difference, fast decay."""
    n = int(length_s * SR)
    rng = np.random.default_rng(0xC0FFEE)  # deterministic
    noise = rng.standard_normal(n)
    noise = np.diff(noise, prepend=0.0)     # emphasise highs
    t = np.arange(n) / SR
    amp = np.exp(-t / 0.03)
    s = amp * noise
    return 0.35 * s / (np.max(np.abs(s)) + 1e-9)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bpm",      type=float, default=120.0)
    ap.add_argument("--duration", type=float, default=120.0, help="seconds")
    ap.add_argument("--out",      default="native/media/beat.wav")
    args = ap.parse_args()

    beat_s   = 60.0 / args.bpm
    total    = int(args.duration * SR)
    buf      = np.zeros(total, dtype=np.float32)

    kick_s   = kick(0.35)
    hat_s    = hat(0.08)

    # Place kicks on beats 1,3 and hats on 2,4 for as long as the buffer lasts.
    n_beats = int(args.duration / beat_s)
    for i in range(n_beats):
        start = int(i * beat_s * SR)
        sample = kick_s if (i % 2 == 0) else hat_s
        end = min(start + len(sample), total)
        buf[start:end] += sample[: end - start]

    # Normalise to ~-3 dBFS with a small safety margin.
    peak = np.max(np.abs(buf))
    if peak > 0:
        buf *= 0.7 / peak

    pcm = np.clip(buf * 32767.0, -32768, 32767).astype(np.int16)
    stereo = np.repeat(pcm[:, None], 2, axis=1).tobytes()

    with wave.open(args.out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(stereo)
    print(f"wrote {args.out}: {args.duration:.1f}s @ {args.bpm} BPM "
          f"({n_beats} beats, {total*4} bytes)")


if __name__ == "__main__":
    main()
