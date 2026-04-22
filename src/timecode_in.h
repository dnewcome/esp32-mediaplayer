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

} // namespace timecode_in
