#include "timecode.h"

#include <math.h>
#include <stdlib.h>

#ifdef TIMECODE_NATIVE
#include <unordered_map>
#endif

namespace timecode {

namespace {

// Samples are left-shifted on entry to give headroom for EMA math +
// hysteresis thresholds (matches xwax's scaling; otherwise the tracked
// zero EMA quantizes badly against int16 samples).
constexpr int     SAMPLE_SHIFT     = 16;
constexpr int32_t ZERO_THRESH      = 128 << 16;
constexpr float   ZERO_RC          = 0.001f;   // 1 ms DC-track tc
constexpr int     REF_PEAKS_AVG    = 48;       // cycles; EMA window
constexpr int     VALID_BITS       = 24;       // bits correct before "locked"
constexpr float   SPEED_EMA_ALPHA  = 0.15f;

constexpr uint32_t SWITCH_PHASE    = 0x1;
constexpr uint32_t SWITCH_PRIMARY  = 0x2;
constexpr uint32_t SWITCH_POLARITY = 0x4;

struct Def {
    Format   fmt;
    int      resolution;
    uint32_t bits;
    uint32_t seed;
    uint32_t taps;
    uint32_t length;
    uint32_t safe;
    uint32_t flags;
};

// Constants are reverse-engineered facts about Serato's commercial
// products, not copyrightable expression. Same values appear in xwax,
// traktorripper, and various clean-room DVS implementations.
constexpr Def kDefs[] = {
    { Format::SeratoControlVinyl, 1000, 20, 0x59017, 0x361e4, 712000, 707000, 0 },
    { Format::SeratoControlCD,    1000, 20, 0xd8b40, 0x34d54, 950000, 940000, 0 },
};

const Def& findDef(Format f) {
    for (const auto& d : kDefs) if (d.fmt == f) return d;
    return kDefs[0];
}

// LFSR parity of (code AND taps). Produces one bit.
inline uint32_t lfsrParity(uint32_t code, uint32_t taps) {
    uint32_t x = code & taps;
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1u;
}

// Advance: new bit enters at MSB, state shifts toward LSB.
inline uint32_t lfsrFwd(uint32_t cur, uint32_t taps, uint32_t bits) {
    uint32_t b = lfsrParity(cur, taps | 1u);
    return (cur >> 1) | (b << (bits - 1));
}

// Rewind: new bit enters at LSB, state shifts toward MSB. Symmetric
// with lfsrFwd so that rev(fwd(x)) == x for every x in the sequence.
inline uint32_t lfsrRev(uint32_t cur, uint32_t taps, uint32_t bits) {
    uint32_t mask = (1u << bits) - 1u;
    uint32_t b = lfsrParity(cur, (taps >> 1) | (1u << (bits - 1)));
    return ((cur << 1) & mask) | b;
}

#ifdef TIMECODE_NATIVE
struct Lut {
    std::unordered_map<uint32_t, uint32_t> map;
    bool built = false;
};
Lut g_luts[sizeof(kDefs) / sizeof(Def)];

Lut& lutFor(Format f) {
    for (size_t i = 0; i < sizeof(kDefs) / sizeof(Def); ++i)
        if (kDefs[i].fmt == f) return g_luts[i];
    return g_luts[0];
}

void buildLut(const Def& d, Lut& out) {
    if (out.built) return;
    out.map.reserve(d.length * 2);
    uint32_t cur = d.seed;
    for (uint32_t i = 0; i < d.length; ++i) {
        out.map.emplace(cur, i);
        cur = lfsrFwd(cur, d.taps, d.bits);
    }
    out.built = true;
}
#endif

int32_t lookupPosition(Format f, uint32_t bs) {
#ifdef TIMECODE_NATIVE
    const Def& d = findDef(f);
    Lut& lut = lutFor(f);
    buildLut(d, lut);
    auto it = lut.map.find(bs);
    return (it == lut.map.end()) ? -1 : (int32_t)it->second;
#else
    (void)f; (void)bs;
    return -1;  // ESP32 path: LUT storage is future work (needs PSRAM).
#endif
}

}  // namespace

void Decoder::begin(int sampleRate, Format fmt) {
    const Def& d = findDef(fmt);
    sampleRate_ = sampleRate;
    fmt_        = fmt;
    bits_       = d.bits;
    taps_       = d.taps;
    seed_       = d.seed;
    defLength_  = d.length;
    safe_       = d.safe;
    flags_      = d.flags;
    resolution_ = d.resolution;

    const float dt = 1.0f / (float)sampleRate;
    zeroAlpha_        = dt / (ZERO_RC + dt);
    threshold_        = ZERO_THRESH;
    refPeriodSamples_ = (float)sampleRate / (float)resolution_;

    reset();
}

void Decoder::reset() {
    primary_   = Channel{};
    secondary_ = Channel{};
    samplesSincePrimaryZC_ = 0;
    forwards_       = true;
    refLevel_       = 0;
    bitstream_      = 0;
    expected_       = 0;
    validCounter_   = 0;
    timecodeTicker_ = 0;
    speed_          = 0.0f;
    position_       = -1;
    locked_         = false;
}

void Decoder::pushFrames(const int16_t* stereo, int frames) {
    const bool swapPrimary = (flags_ & SWITCH_PRIMARY) != 0;
    for (int i = 0; i < frames; ++i) {
        int32_t l = (int32_t)stereo[i * 2]     << SAMPLE_SHIFT;
        int32_t r = (int32_t)stereo[i * 2 + 1] << SAMPLE_SHIFT;
        // Default (no SWITCH_PRIMARY): primary = R, secondary = L.
        if (swapPrimary) processSample(l, r);
        else             processSample(r, l);
    }
}

void Decoder::updateChannel(Channel& ch, int32_t v, float alpha, int32_t thr) {
    ch.crossingTicker++;
    ch.swapped = false;
    if (v > ch.zero + thr && !ch.positive) {
        ch.swapped = true;
        ch.positive = true;
        ch.crossingTicker = 0;
    } else if (v < ch.zero - thr && ch.positive) {
        ch.swapped = true;
        ch.positive = false;
        ch.crossingTicker = 0;
    }
    ch.zero += (int32_t)(alpha * (float)(v - ch.zero));
}

void Decoder::processSample(int32_t primary, int32_t secondary) {
    updateChannel(primary_,   primary,   zeroAlpha_, threshold_);
    updateChannel(secondary_, secondary, zeroAlpha_, threshold_);

    // Direction: decided at any ZC by comparing the polarity relationship
    // between the two channels. When primary is the one that just crossed,
    // forward ↔ channels differ in polarity; when secondary crossed, it's
    // the opposite. (Proof: cos-after-sin-cross vs sin-after-cos-cross.)
    if (primary_.swapped || secondary_.swapped) {
        bool fwd;
        if (primary_.swapped) {
            fwd = (primary_.positive != secondary_.positive);
        } else {
            fwd = (primary_.positive == secondary_.positive);
        }
        if (flags_ & SWITCH_PHASE) fwd = !fwd;
        if (fwd != forwards_) {
            forwards_ = fwd;
            validCounter_ = 0;  // direction change invalidates bit history
        }
    }

    // Speed from primary-channel period (measured between consecutive ZCs
    // = one half-cycle, so multiply by 2 for full-carrier period).
    samplesSincePrimaryZC_++;
    if (primary_.swapped) {
        uint32_t halfPeriod = samplesSincePrimaryZC_;
        samplesSincePrimaryZC_ = 0;
        if (halfPeriod > 0) {
            float ratio = refPeriodSamples_ / (2.0f * (float)halfPeriod);
            float s     = forwards_ ? ratio : -ratio;
            speed_ = (1.0f - SPEED_EMA_ALPHA) * speed_ + SPEED_EMA_ALPHA * s;
        }
    }

    // Bit sample: on secondary's ZC, when primary is in the configured
    // polarity. Magnitude vs ref_level → 1 bit.
    const bool polarity = (flags_ & SWITCH_POLARITY) == 0;
    if (secondary_.swapped && primary_.positive == polarity) {
        int32_t m = abs(primary / 2 - primary_.zero / 2);
        processBitstream(m);
    }

    timecodeTicker_++;

    // Silence gate: no zero-crossings on either channel for 100 ms → stopped.
    const uint32_t silenceWindow = (uint32_t)(sampleRate_ / 10);
    if (primary_.crossingTicker   > silenceWindow
     && secondary_.crossingTicker > silenceWindow) {
        speed_  = 0.0f;
        locked_ = false;
        return;
    }

    // "locked" = enough predicted bits matched AND speed in sane DJ range.
    locked_ = (validCounter_ > VALID_BITS)
              && (fabsf(speed_) > 0.05f) && (fabsf(speed_) < 4.0f);
}

void Decoder::processBitstream(int32_t m) {
    // Start ref_level high so the first bit reads 0, then the EMA walks
    // it down to the actual peak magnitude over ~REF_PEAKS_AVG cycles.
    if (refLevel_ == 0) refLevel_ = INT32_MAX / 2;

    const uint32_t b = (m > refLevel_) ? 1u : 0u;

    if (forwards_) {
        expected_  = lfsrFwd(expected_, taps_, bits_);
        bitstream_ = (bitstream_ >> 1) | (b << (bits_ - 1));
    } else {
        const uint32_t mask = (1u << bits_) - 1u;
        expected_  = lfsrRev(expected_, taps_, bits_);
        bitstream_ = ((bitstream_ << 1) & mask) | b;
    }

    if (expected_ == bitstream_) {
        validCounter_++;
    } else {
        // Re-sync: trust the observed bitstream, restart confidence counter.
        expected_     = bitstream_;
        validCounter_ = 0;
    }

    timecodeTicker_ = 0;

    // ref_level is an EMA of recent peak magnitudes.
    refLevel_ -= refLevel_ / REF_PEAKS_AVG;
    refLevel_ += m         / REF_PEAKS_AVG;

    if (validCounter_ > VALID_BITS) {
        int32_t p = lookupPosition(fmt_, bitstream_);
        if (p >= 0) position_ = p;
    }
}

} // namespace timecode
