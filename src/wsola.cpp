#include "wsola.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace wsola {

namespace {

inline int16_t sat16(int32_t x) {
    if (x >  32767) return  32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

inline int32_t absDiff(int32_t a, int32_t b) {
    int32_t d = a - b;
    return d < 0 ? -d : d;
}

} // namespace

// ===========================================================================
// Wsola core DSP
// ===========================================================================

void Wsola::begin(int channels) {
    channels_ = (channels == 1) ? 1 : 2;
    reset();
    initHann();
}

void Wsola::setSpeed(float s) {
    if (s < 0.25f) s = 0.25f;
    if (s > 4.0f)  s = 4.0f;
    speed_ = s;
}

void Wsola::reset() {
    inFill_  = 0;
    anaPos_  = 0.0;
    prevAnaStart_ = -1;
    outHead_ = 0;
    outFill_ = 0;
    memset(olaBuf_, 0, sizeof(olaBuf_));
}

void Wsola::initHann() {
    if (hannReady_) return;
    for (int i = 0; i < FRAME_N; ++i) {
        float w = 0.5f - 0.5f * cosf((2.0f * (float)M_PI * (float)i) / (float)(FRAME_N - 1));
        int32_t q = (int32_t)lroundf(w * 32767.0f);
        if (q > 32767) q = 32767;
        if (q < 0)     q = 0;
        hann_[i] = (int16_t)q;
    }
    hannReady_ = true;
}

int Wsola::inputCapacity() const {
    return IN_CAP - inFill_;
}

int Wsola::outputAvailable() const {
    return outFill_ - outHead_;
}

int Wsola::pushFrames(const int16_t* in, int frames) {
    const int cap = inputCapacity();
    if (frames > cap) frames = cap;
    if (frames <= 0) return 0;

    const int ch = channels_;
    int16_t* dst = inBuf_ + inFill_ * 2;
    int16_t* dstMono = inMono_ + inFill_;

    if (ch == 2) {
        memcpy(dst, in, frames * 2 * sizeof(int16_t));
        for (int i = 0; i < frames; ++i) {
            int32_t L = in[i * 2];
            int32_t R = in[i * 2 + 1];
            dstMono[i] = (int16_t)((L + R) >> 1);
        }
    } else {
        // Mono input: duplicate to both channels in the stereo buffer so the
        // rest of the algorithm can operate unconditionally as stereo.
        for (int i = 0; i < frames; ++i) {
            int16_t s = in[i];
            dst[i * 2]     = s;
            dst[i * 2 + 1] = s;
            dstMono[i]     = s;
        }
    }
    inFill_ += frames;
    return frames;
}

int Wsola::pullFrames(int16_t* out, int frames) {
    int produced = 0;
    while (produced < frames) {
        int ready = outputAvailable();
        if (ready > 0) {
            int n = frames - produced;
            if (n > ready) n = ready;
            memcpy(out + produced * 2,
                   outBuf_ + outHead_ * 2,
                   n * 2 * sizeof(int16_t));
            outHead_ += n;
            produced += n;
            if (outHead_ == outFill_) { outHead_ = 0; outFill_ = 0; }
            continue;
        }
        if (!canProduceFrame()) break;
        produceFrame();
    }
    return produced;
}

bool Wsola::canProduceFrame() const {
    const int nominal = (int)anaPos_;
    // Need range [nominal - SEARCH, nominal + SEARCH + FRAME_N) in inBuf_.
    // If nominal < SEARCH we fall back to a clamped search (handled at call
    // time), so only the right-hand requirement gates production.
    const int needHigh = nominal + SEARCH + FRAME_N;
    if (needHigh > inFill_) return false;
    // Output slot for HOP_SYN new frames.
    if (outFill_ + HOP_SYN > OUT_CAP) return false;
    return true;
}

int Wsola::findBestOffset(int nominal) {
    if (prevAnaStart_ < 0) return 0;

    // Reference: the "natural continuation" of the previous analysis frame
    // at speed 1.0 — i.e., the HOP_SYN samples starting HOP_SYN into the
    // previous frame. This is what would have followed if we hadn't stretched.
    const int ref = prevAnaStart_ + HOP_SYN;
    if (ref < 0 || ref + HOP_SYN > inFill_) return 0;

    // Clamp search bounds to valid buffer positions.
    int loOff = -SEARCH;
    int hiOff =  SEARCH;
    if (nominal + loOff < 0)           loOff = -nominal;
    if (nominal + hiOff + HOP_SYN > inFill_) hiOff = inFill_ - HOP_SYN - nominal;
    if (hiOff < loOff) return 0;

    int     bestOff   = 0;
    int32_t bestScore = INT32_MAX;
    const int16_t* mono = inMono_;
    const int16_t* refP = mono + ref;

    for (int off = loOff; off <= hiOff; ++off) {
        const int16_t* candP = mono + nominal + off;
        int32_t score = 0;
        for (int i = 0; i < HOP_SYN; ++i) {
            score += absDiff((int32_t)candP[i], (int32_t)refP[i]);
            // Early-exit pruning — no need to finish if we've already lost.
            if (score >= bestScore) break;
        }
        if (score < bestScore) {
            bestScore = score;
            bestOff   = off;
        }
    }
    return bestOff;
}

void Wsola::produceFrame() {
    const int nominal = (int)anaPos_;
    const int offset  = findBestOffset(nominal);
    const int start   = nominal + offset;

    // --- window the selected input frame into a member scratch buffer ---
    // Stereo interleaved * Q15 Hann.
    int16_t* windowed = windowedScratch_;
    const int16_t* src = inBuf_ + start * 2;
    for (int i = 0; i < FRAME_N; ++i) {
        int16_t w = hann_[i];
        int32_t L = (int32_t)src[i * 2]     * (int32_t)w;
        int32_t R = (int32_t)src[i * 2 + 1] * (int32_t)w;
        windowed[i * 2]     = (int16_t)(L >> 15);
        windowed[i * 2 + 1] = (int16_t)(R >> 15);
    }

    // --- emit HOP_SYN output frames from olaBuf_[0..HOP_SYN) + windowed[0..HOP_SYN) ---
    int16_t* outPtr = outBuf_ + outFill_ * 2;
    for (int i = 0; i < HOP_SYN; ++i) {
        int32_t sumL = olaBuf_[i * 2]     + (int32_t)windowed[i * 2];
        int32_t sumR = olaBuf_[i * 2 + 1] + (int32_t)windowed[i * 2 + 1];
        outPtr[i * 2]     = sat16(sumL);
        outPtr[i * 2 + 1] = sat16(sumR);
    }
    outFill_ += HOP_SYN;

    // --- shift olaBuf_ left by HOP_SYN, zero the new tail ---
    memmove(olaBuf_,
            olaBuf_ + HOP_SYN * 2,
            (OLA_LEN - HOP_SYN) * 2 * sizeof(int32_t));
    memset(olaBuf_ + (OLA_LEN - HOP_SYN) * 2,
           0,
           HOP_SYN * 2 * sizeof(int32_t));

    // --- accumulate windowed[HOP_SYN..FRAME_N) into olaBuf_ ---
    for (int i = 0; i < OLA_LEN; ++i) {
        olaBuf_[i * 2]     += (int32_t)windowed[(HOP_SYN + i) * 2];
        olaBuf_[i * 2 + 1] += (int32_t)windowed[(HOP_SYN + i) * 2 + 1];
    }

    // --- advance analysis position ---
    prevAnaStart_ = start;
    anaPos_      += (double)HOP_SYN * (double)speed_;

    // --- compact input buffer when we're far from the start ---
    // Keep a safety margin behind anaPos_ so the next findBestOffset reference
    // (prevAnaStart + HOP_SYN, + search range) still has data.
    const int keepBehind = HOP_SYN + SEARCH + 128;
    int shiftBy = (int)anaPos_ - keepBehind;
    if (shiftBy > IN_CAP / 4) {
        if (shiftBy > inFill_) shiftBy = inFill_;
        const int remaining = inFill_ - shiftBy;
        memmove(inBuf_,  inBuf_  + shiftBy * 2, remaining * 2 * sizeof(int16_t));
        memmove(inMono_, inMono_ + shiftBy,     remaining * sizeof(int16_t));
        inFill_       -= shiftBy;
        anaPos_       -= (double)shiftBy;
        prevAnaStart_ -= shiftBy;
    }
}

// ===========================================================================
// WsolaStream — arduino-audio-tools AudioStream adapter
// ===========================================================================

void WsolaStream::begin(int /*sampleRate*/, int channels) {
    channels_ = (channels == 1) ? 1 : 2;
    dsp_.begin(channels_);
}

int WsolaStream::availableForWrite() {
    return dsp_.inputCapacity() * channels_ * (int)sizeof(int16_t);
}

size_t WsolaStream::write(const uint8_t* data, size_t len) {
    const int bytesPerFrame = channels_ * (int)sizeof(int16_t);
    int framesAvail = (int)(len / bytesPerFrame);

    int pushed = dsp_.pushFrames(reinterpret_cast<const int16_t*>(data), framesAvail);
    drainToSink();
    return (size_t)(pushed * bytesPerFrame);
}

void WsolaStream::drainToSink() {
    const int bytesPerFrame = channels_ * (int)sizeof(int16_t);
    while (true) {
        int n = dsp_.pullFrames(pullBuf_, PULL_CHUNK);
        if (n <= 0) break;
        sink_.write(reinterpret_cast<const uint8_t*>(pullBuf_), (size_t)(n * bytesPerFrame));
    }
}

} // namespace wsola
