#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef TIMECODE_NATIVE
#include <Arduino.h>
#endif

// Timecode vinyl/CD decoder (Serato Control Vinyl / Control CD).
//
// Clean-room implementation of the xwax (http://xwax.org/) algorithm —
// xwax is GPL-3, so the code here is not copied; only the published
// timecode format constants (reverse-engineered facts about Serato's
// products) are used as data.
//
// Pipeline:
//
//   int16 stereo in ──► per-channel ZC + DC-tracking + hysteresis
//                       │                                  │
//                       ▼                                  ▼
//                 direction (primary vs              period → speed
//                  secondary polarity at ZC)
//                       │                                  │
//                       ▼                                  │
//             bit sample at secondary ZC                   │
//             (loud vs quiet → 1 vs 0)                     │
//                       │                                  │
//                       ▼                                  │
//             LFSR predictor + valid_counter               │
//                       │                                  │
//                       ▼                                  │
//             LUT lookup ► absolute cycle position ◄───────┘
//
// On host builds (TIMECODE_NATIVE) the LUT is a std::unordered_map built
// lazily on first decode. On ESP32 the LUT is currently unavailable —
// position() returns -1; speed() and direction still work. A PSRAM-
// resident compact LUT is future work.

namespace timecode {

enum class Format : uint8_t {
    SeratoControlVinyl,
    SeratoControlCD,
};

class Decoder {
public:
    void begin(int sampleRate, Format fmt = Format::SeratoControlCD);
    void reset();

    void pushFrames(const int16_t* stereo, int frames);

    // Sign = direction, magnitude = period ratio vs reference.
    float    speed() const    { return speed_; }

    // Cycle-indexed absolute position on the timecode, or -1 until we've
    // seen VALID_BITS consecutive predicted bits. Convert to wall time
    // with position() / resolutionHz().
    int32_t  position() const { return position_; }

    bool     locked() const   { return locked_; }

    int      resolutionHz() const { return resolution_; }

private:
    struct Channel {
        bool     positive       = false;
        bool     swapped        = false;  // crossed this sample
        int32_t  zero           = 0;      // EMA-tracked DC offset
        uint32_t crossingTicker = 0;      // samples since last ZC
    };

    int      sampleRate_ = 44100;
    Format   fmt_        = Format::SeratoControlCD;

    // Format-derived constants (filled by begin()).
    uint32_t bits_       = 20;
    uint32_t taps_       = 0;
    uint32_t seed_       = 0;
    uint32_t defLength_  = 0;
    uint32_t safe_       = 0;
    uint32_t flags_      = 0;
    int      resolution_ = 1000;

    float    zeroAlpha_       = 0.0f;
    int32_t  threshold_       = 0;
    float    refPeriodSamples_ = 44.1f;

    Channel  primary_, secondary_;

    uint32_t samplesSincePrimaryZC_ = 0;
    bool     forwards_ = true;

    int32_t  refLevel_       = 0;
    uint32_t bitstream_      = 0;
    uint32_t expected_       = 0;
    uint32_t validCounter_   = 0;
    uint32_t timecodeTicker_ = 0;

    float    speed_    = 0.0f;
    int32_t  position_ = -1;
    bool     locked_   = false;

    void processSample(int32_t primary, int32_t secondary);
    void processBitstream(int32_t mag);
    static void updateChannel(Channel& ch, int32_t v, float alpha, int32_t thr);
};

} // namespace timecode
