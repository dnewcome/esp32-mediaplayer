#include "timecode.h"
#include <math.h>
#include <stdlib.h>

namespace timecode {

namespace {

// Carrier frequencies for the supported formats. Treated as nominal — the
// zero-crossing estimator measures the actual period per half-cycle.
float carrierHz(Format f) {
    switch (f) {
        case Format::SeratoControlVinyl: return 1000.0f;
        case Format::SeratoControlCD:    return 1000.0f;
    }
    return 1000.0f;
}

// Smoothing factor for speed EMA. Larger = snappier but noisier.
constexpr float SPEED_EMA_ALPHA = 0.15f;

// Samples between direction re-estimations (stable enough to drive UI).
constexpr uint32_t DIR_CHECK_INTERVAL = 128;

// Minimum carrier amplitude below which the decoder considers the signal
// silent (stylus lifted). In int16 terms.
constexpr int16_t SILENCE_THRESH = 200;

} // namespace

void Decoder::begin(int sampleRate, Format fmt) {
    sampleRate_        = sampleRate;
    fmt_               = fmt;
    refPeriodSamples_  = (float)sampleRate / carrierHz(fmt);
    reset();
}

void Decoder::reset() {
    zcL_ = zcR_ = ZCState{};
    dirSign_  = +1;
    samplesSinceLastDirCheck_ = 0;
    speed_    = 0.0f;
    position_ = -1;
    locked_   = false;
}

void Decoder::pushFrames(const int16_t* stereo, int frames) {
    for (int i = 0; i < frames; ++i) {
        processSample(stereo[i * 2], stereo[i * 2 + 1]);
    }
}

void Decoder::processSample(int16_t l, int16_t r) {
    // Silence gate: if both channels are tiny, treat as stopped.
    if (abs(l) < SILENCE_THRESH && abs(r) < SILENCE_THRESH) {
        speed_ = 0.0f;
        locked_ = false;
        zcL_.samplesSinceZC++;
        zcR_.samplesSinceZC++;
        return;
    }

    auto advance = [](ZCState& st, int16_t sample) -> bool {
        st.samplesSinceZC++;
        const bool prevSign = st.lastSample >= 0;
        const bool curSign  = sample >= 0;
        st.lastSample = sample;
        if (prevSign != curSign) {
            // Record period as 2× half-cycle for full-carrier-period estimate.
            uint32_t halfPeriod = st.samplesSinceZC;
            st.samplesSinceZC   = 0;
            if (st.hadFirst) {
                st.lastPeriod = halfPeriod * 2;
                return true;
            }
            st.hadFirst = true;
        }
        return false;
    };

    const bool zcL = advance(zcL_, l);
    const bool zcR = advance(zcR_, r);

    if (zcL) updateSpeedFromPeriod(zcL_.lastPeriod);
    if (zcR) updateSpeedFromPeriod(zcR_.lastPeriod);

    samplesSinceLastDirCheck_++;
    if (samplesSinceLastDirCheck_ >= DIR_CHECK_INTERVAL) {
        samplesSinceLastDirCheck_ = 0;
        updateDirection(l, r);
    }
}

void Decoder::updateSpeedFromPeriod(uint32_t period) {
    if (period == 0) return;
    const float ratio     = refPeriodSamples_ / (float)period;  // >1 = faster
    const float signed_   = ratio * (float)dirSign_;
    speed_ = (1.0f - SPEED_EMA_ALPHA) * speed_ + SPEED_EMA_ALPHA * signed_;

    // Lock criterion: speed within a reasonable DJ range. No bit-decode yet,
    // so "locked" just means we're tracking a plausible carrier.
    locked_ = (fabsf(speed_) > 0.05f && fabsf(speed_) < 4.0f);
}

void Decoder::updateDirection(int16_t l, int16_t r) {
    // Quadrature: at forward playback, one channel leads the other by 90°.
    // Instantaneous sign(L) * sign(dR/dt) gives a consistent polarity for
    // forward rotation; here we approximate using raw sample sign of both,
    // which gives a coarse but usually-correct reading.
    //
    // NOTE: which channel-leads-which for "forward" depends on cart wiring —
    // may need a calibration step in the UI. Treating +1 as default forward.
    const int32_t cross = (int32_t)l * (int32_t)r;
    if (cross > 0) dirSign_ = +1;
    else if (cross < 0) dirSign_ = -1;
    // cross == 0: keep previous
}

// ---------------------------------------------------------------------------
// TODO — not yet implemented:
//
// 1. Bit extractor:
//      - Sample the carrier envelope at each zero-crossing to capture the
//        NRZI bit value (amplitude modulated onto the carrier).
//      - Run a simple PLL to hold phase against carrier jitter.
//
// 2. Code table lookup:
//      - Serato Control Vinyl uses an LFSR-derived pseudorandom sequence.
//        xwax has reverse-engineered tables (see xwax/timecoder.c, "serato_2a"
//        and friends). Port the relevant table into a flash-resident array.
//      - Match last N received bits against the table; N typically 22–24 for
//        1.2 second resolution across a full side.
//
// 3. Wire into player:
//      - Add a source mode flag to player: File vs. Timecode-driven.
//      - In timecode mode, do NOT consume the file via copier.copy(); instead,
//        periodically seek the file to the sample position reported by the
//        decoder and play a small chunk per iteration, with playback rate set
//        by decoder speed().
//      - The codec must be initialized for simultaneous TX+RX: line-in feeds
//        the Decoder (via a second stream) while the player outputs to the
//        codec's DAC.
// ---------------------------------------------------------------------------

} // namespace timecode
