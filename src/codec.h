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

// TX data pipeline wrapper: player writes PCM *here* instead of straight
// to kit(). Every write is scanned for peak amplitude and forwarded to
// kit() unchanged. Lets us log what the DAC is producing independently
// of what the ADC reports, so we can tell DAC-bleed (RX peak tracks TX
// peak) from some other coupling (RX peak rises without TX).
AudioStream& txSink();

// Peak |sample| observed on TX since last call. Reset-on-read.
int16_t  takeTxPeak();

// Diagnostic: how many times the TX path was written to since last
// call. Stays 0 if the decoder bypasses the tap somehow.
uint32_t takeTxWriteCount();

// Idempotent: safe to call more than once. First call configures the
// codec for full-duplex at cfg::SAMPLE_RATE / CHANNELS / BITS_PER_SAMPLE.
void begin();

// Direct I²C access to ES8388 registers (bus shared with the library).
// Used by bring-up diagnostics to snapshot codec state — e.g. to diff
// registers before vs. after starting playback and catch whatever's
// pinning the ADC at full scale.
uint8_t readReg(uint8_t reg);
void    writeReg(uint8_t reg, uint8_t val);
void    dumpRegs();

// Bump the ADC input PGA up (positive delta) or down (negative).
// Bring-up knob for matching gain to whatever is feeding line-in.
void    adjustInputGain(int delta);

// Adjust DAC output volume. Used during bring-up to check whether
// DAC→ADC bleed is the dominant noise source (drop output, measure
// whether ADC peak drops in proportion).
void    adjustOutputVolume(int delta);

} // namespace codec
