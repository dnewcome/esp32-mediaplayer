#pragma once
#include <Arduino.h>
#include <stdint.h>

// Timecode vinyl decoder (Serato Control Vinyl / CD).
//
// High-level design (closely mirrors xwax, https://xwax.org/):
//
//     stereo audio in ──► zero-crossing detector ──► speed estimator
//                              │                        │
//                              ▼                        ▼
//                        direction (L/R phase)    position estimator
//                              │                        │
//                              ▼                        ▼
//                         bit extractor ──► PLL ──► timecode lookup ──► position
//
// The carrier is ~1 kHz with 90° L/R phase quadrature — zero crossings give
// speed (period between crossings) and direction (which channel leads). An
// NRZI bit stream is amplitude-modulated on top, and a lookup table
// decodes the absolute position within the track.
//
// THIS FILE IS A SKELETON:
//   - Zero-crossing + speed estimation: implemented (validatable with a
//     synthetic sine-sweep on a host build).
//   - Direction detection: implemented (quadrature sign).
//   - Bit extraction + position lookup: NOT IMPLEMENTED. Requires Serato's
//     specific code table, which is reverse-engineered and best adopted from
//     xwax's tables once we can test against real audio. Left as TODO.
//
// Outputs are intended to drive the player:
//   - speed()     → player::setSpeed()
//   - position()  → player seeks to corresponding file offset (once available)
//   - locked()    → only act on position/speed when true

namespace timecode {

// Supported formats; add more as decoders are written.
enum class Format : uint8_t {
    SeratoControlVinyl,   // default for this project
    SeratoControlCD,
};

class Decoder {
public:
    void  begin(int sampleRate, Format fmt = Format::SeratoControlVinyl);
    void  reset();

    // Feed interleaved stereo int16 samples from the codec's ADC.
    void  pushFrames(const int16_t* stereo, int frames);

    // Most recent speed estimate. Sign encodes direction:
    //   +1.0 = forward at reference pitch
    //    0.0 = stopped
    //   -1.0 = reverse at reference pitch
    // Magnitude tracks the ratio of measured carrier period to the reference
    // period (33⅓ RPM). No smoothing yet — caller may low-pass if needed.
    float speed() const { return speed_; }

    // Absolute sample position within the track, or -1 if not yet locked.
    // (Bit-decode stage not implemented — always returns -1 for now.)
    int32_t position() const { return position_; }

    bool locked() const { return locked_; }

private:
    int      sampleRate_ = 44100;
    Format   fmt_        = Format::SeratoControlVinyl;

    // Zero-crossing tracking (per channel).
    struct ZCState {
        int16_t  lastSample  = 0;
        uint32_t samplesSinceZC = 0;
        uint32_t lastPeriod  = 0;   // samples between consecutive same-sign crossings
        bool     hadFirst    = false;
    };
    ZCState zcL_, zcR_;

    // Phase relationship sample: +1 if L leads R, -1 if R leads L.
    int8_t   dirSign_ = +1;
    uint32_t samplesSinceLastDirCheck_ = 0;

    // Outputs.
    float    speed_    = 0.0f;
    int32_t  position_ = -1;
    bool     locked_   = false;

    // Reference carrier period at speed 1.0 (samples).
    // Serato Control Vinyl carrier ≈ 1000 Hz → 44.1 samples @ 44.1 kHz.
    float    refPeriodSamples_ = 44.1f;

    void processSample(int16_t l, int16_t r);
    void updateSpeedFromPeriod(uint32_t period);
    void updateDirection(int16_t l, int16_t r);
};

} // namespace timecode
