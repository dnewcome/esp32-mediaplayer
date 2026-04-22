#pragma once

// The AudioBoardStream (codec + I²S peripheral) is shared: `player` owns
// the TX path (decoded audio out), and `timecode_in` reads the RX path
// (line-in signal → timecode decoder). Both sides need the same
// instance, so it lives here instead of inside either module.
//
// The kit is configured RXTX_MODE so the codec's ADC captures the
// line-in jack into the I²S RX FIFO at the same sample rate the player
// uses for TX.

#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"

namespace codec {

AudioBoardStream& kit();

// Idempotent: safe to call more than once. First call configures the
// codec for full-duplex at cfg::SAMPLE_RATE / CHANNELS / BITS_PER_SAMPLE.
void begin();

} // namespace codec
