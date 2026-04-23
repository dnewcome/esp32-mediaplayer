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

// Decoder convention flags (used with Decoder::setFlags). The format
// baseline assumes the xwax canonical signal chain; real hardware may
// need one or more of these to match its wiring:
//   SWITCH_PHASE    — invert direction decision at zero crossings
//   SWITCH_PRIMARY  — swap which channel is primary vs secondary (L↔R)
//   SWITCH_POLARITY — flip bit polarity (loud vs quiet → 1 vs 0)
constexpr uint32_t SWITCH_PHASE    = 0x1;
constexpr uint32_t SWITCH_PRIMARY  = 0x2;
constexpr uint32_t SWITCH_POLARITY = 0x4;

enum class Format : uint8_t {
    SeratoControlVinyl,
    SeratoControlCD,
};

// Eagerly build the position LUT for `f` (PSRAM-backed on ESP32).
// Safe to call from any context; no-op after the first successful
// build for that format. Call from setup() to avoid the ~2.5 s stall
// that otherwise hits the first call to Decoder::pushFrames on the
// tc task. No-op on host builds (which use a std::unordered_map).
void prebuildLut(Format f);

// Rewrite the existing LUT buffer with entries for `f`. Caller must
// guarantee no reader is active (on ESP32, suspend the tc task).
// Skips allocation — reuses the 3 MB PSRAM buffer built for the
// initial format. No-op if prebuildLut hasn't run yet or on host.
void rebuildLutInPlace(Format f);

// Nominal total duration of the timecode media in milliseconds,
// derived from format length × (1000 / resolutionHz). Vinyl: 712000,
// CD: 950000. Used by the seek-from-position mapping.
uint32_t totalDurationMs(Format f);

class Decoder {
public:
    void begin(int sampleRate, Format fmt = Format::SeratoControlCD);
    void reset();

    // Convention override. The format-table baseline covers the xwax
    // canonical decode; real-world signals (channel swaps in the codec
    // or cabling, opposite direction convention, inverted bit polarity)
    // can need one or more of SWITCH_PRIMARY/SWITCH_PHASE/SWITCH_POLARITY
    // flipped. Replaces flags wholesale and resets decoder state.
    void     setFlags(uint32_t flags);
    uint32_t flags() const { return flags_; }

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
