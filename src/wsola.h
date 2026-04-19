#pragma once
#include <Arduino.h>
#include <stdint.h>

#include "AudioTools.h"

// Waveform-Similarity Overlap-Add time stretching.
//
// Time-domain algorithm for playing audio faster or slower without changing
// pitch (keylock). Classical formulation (Verhelst & Roelands, 1993):
//
//   - Extract analysis windows from the input at hop Ha = Hs * speed.
//   - To avoid phase discontinuities at frame boundaries, search within ±Δ of
//     each nominal analysis position for the window whose leading Hs samples
//     best match the "natural continuation" of the previous analysis window
//     (the samples that would have followed it at speed 1.0).
//   - Hann-window each chosen frame and overlap-add at synthesis hop Hs.
//
// Tuning (44.1 kHz music, ESP32 compute budget):
//   FRAME_N = 1024  (~23 ms)
//   HOP_SYN =  256  (~5.8 ms, 75% overlap)
//   SEARCH  =  128  (~2.9 ms each side)
//
// AMDF similarity (abs-diff, no multiplies) is used for the search — cheaper
// than normalized cross-correlation and good enough for music. Search runs
// on a mono-summed cache of the input; overlap-add operates on full stereo.

namespace wsola {

class Wsola {
public:
    static constexpr int FRAME_N = 1024;
    static constexpr int HOP_SYN = 256;
    static constexpr int SEARCH  = 128;

    void  begin(int channels);           // 1 or 2; other values treated as 2
    void  setSpeed(float s);             // clamped to [0.25, 4.0]
    float speed() const { return speed_; }
    void  reset();

    // Push interleaved int16 frames (1 frame = `channels` int16 samples).
    // Returns frames accepted (may be < `frames` if buffer is full).
    int pushFrames(const int16_t* in, int frames);

    // Pull interleaved int16 frames. Returns frames written (may be < `frames`
    // if not enough input has been buffered to produce more output).
    int pullFrames(int16_t* out, int frames);

    int inputCapacity() const;    // frames still acceptable
    int outputAvailable() const;  // frames ready to read

private:
    // --- configuration ---
    int   channels_ = 2;
    float speed_    = 1.0f;

    // --- input (linear; compacts toward index 0 as frames are consumed) ---
    static constexpr int IN_CAP = 4096;
    int16_t inBuf_[IN_CAP * 2];
    int16_t inMono_[IN_CAP];   // (L+R)/2 cached for AMDF search
    int     inFill_ = 0;

    // --- analysis state ---
    // Fractional analysis position in inBuf_ coordinates.
    double  anaPos_ = 0.0;
    // Start index (in inBuf_ coords) of the previous chosen frame; -1 = none.
    int     prevAnaStart_ = -1;

    // --- overlap-add accumulator ---
    // Holds the "future" portion of past windowed frames, length
    // (FRAME_N - HOP_SYN) frames. int32 to avoid overflow during OLA summation.
    static constexpr int OLA_LEN = FRAME_N - HOP_SYN;
    int32_t olaBuf_[OLA_LEN * 2];

    // --- output buffer ---
    static constexpr int OUT_CAP = FRAME_N;
    int16_t outBuf_[OUT_CAP * 2];
    int     outHead_ = 0;
    int     outFill_ = 0;

    // --- precomputed Hann window, Q15 ---
    int16_t hann_[FRAME_N];
    bool    hannReady_ = false;

    // --- scratch for a windowed analysis frame (member, not stack, to avoid
    //     blowing the ~8KB ESP32 task stack) ---
    int16_t windowedScratch_[FRAME_N * 2];

    // --- helpers ---
    void initHann();
    void compactInput();
    bool canProduceFrame() const;
    void produceFrame();
    int  findBestOffset(int nominal);
};

// ---------------------------------------------------------------------------
// Audio-tools adapter: wraps Wsola in an AudioStream so the player's copier
// can feed bytes into it, and it forwards stretched samples to a downstream
// sink (the I2S/codec stream).
// ---------------------------------------------------------------------------

class WsolaStream : public AudioStream {
public:
    explicit WsolaStream(AudioStream& sink) : sink_(sink) {}

    void begin(int sampleRate, int channels);
    void setSpeed(float s) { dsp_.setSpeed(s); }
    void reset()           { dsp_.reset(); }

    size_t write(const uint8_t* data, size_t len) override;
    int    availableForWrite() override;

    // Sink-only: reading from this stream isn't meaningful.
    size_t readBytes(uint8_t* /*data*/, size_t /*len*/) override { return 0; }
    int    available() override { return 0; }

private:
    AudioStream& sink_;
    Wsola        dsp_;
    int          channels_ = 2;

    static constexpr int PULL_CHUNK = 256;   // frames per sink drain cycle
    int16_t pullBuf_[PULL_CHUNK * 2];

    void drainToSink();
};

} // namespace wsola
