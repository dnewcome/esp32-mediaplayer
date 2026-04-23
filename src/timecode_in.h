#pragma once
#include <stdint.h>

// Pulls audio frames from the codec's RX path (line-in jack → ADC →
// I²S RX FIFO) and feeds them to the timecode decoder. Phase 1 scope:
// speed + direction only. The `position()` LUT is host-only today and
// returns -1 on ESP32 — that's fine for scratch-style playback control.
//
// The decoder is idle (its inputs are ignored) unless `enabled()` is
// true. Toggle on when a timecode source is physically connected.

namespace timecode_in {

void begin();

// Drain whatever RX frames are available and push them through the
// decoder. Intended to be called from the Arduino main loop — cheap
// when nothing is queued, non-blocking either way.
void tick();

void setEnabled(bool on);
bool enabled();

// Latest decoder state. `speed` is signed (negative = reverse).
// `locked` means the LFSR predictor has seen VALID_BITS correct in a
// row; speed/direction are trustworthy once locked.
float   speed();
bool    locked();
int32_t position();   // -1 on ESP32 (LUT is Phase 2 work)

// Bring-up diagnostics. `peak` is the max |sample| observed across both
// channels since the last takeStats() call; `frames` is how many stereo
// frames tick() drained from the codec in that window. Reading clears
// both, so successive calls show per-window counters. Useful for
// answering "is line-in even seeing signal?" before the decoder locks.
struct Stats { int16_t peak; uint32_t frames; };
Stats   takeStats();

} // namespace timecode_in
