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

// Cycle the decoder's convention flags (SWITCH_PHASE / SWITCH_PRIMARY /
// SWITCH_POLARITY) through 0..7. Useful during bring-up when the input
// signal chain differs from what SWITCH_PRIMARY alone covers — e.g.
// switching from headphone-out to line-out can invert bit polarity.
uint32_t cycleFlags();

// Diagnostic "local-loop" mode. When ON, the decoder is fed stereo PCM
// frames read directly from /timecode.wav on SD, bypassing the ADC and
// the DAC→ADC coupling path entirely. Intended for validating position
// lookup, driveFromTimecode, and seek logic without any hardware
// signal — the result is deterministic: steady speed≈1.0× and
// monotonically-increasing position. Toggle is independent of the
// `enabled()` flag, but both must be ON for frames to reach the
// decoder. Returns false if the file could not be opened.
bool setLocalLoop(bool on);
bool localLoop();

// Cycle decoder format between SeratoControlVinyl and SeratoControlCD.
// Different seeds/taps: bit-lock can't happen cross-format even though
// the carrier frequency is the same. Required to get locked=1 in
// local-loop when /timecode.wav is CD-format. Resets decoder state.
void cycleFormat();
bool isCdFormat();

// Decoder sample-resolution (Hz) and total timecode duration (ms) for
// the currently-selected format. Used by driveFromTimecode to convert
// a cycle-index position into a millisecond time and to scale
// Proportional mapping against the vinyl length.
int      resolutionHz();
uint32_t totalDurationMs();

} // namespace timecode_in
