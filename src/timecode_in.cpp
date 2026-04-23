#include "timecode_in.h"

#include "codec.h"
#include "config.h"
#include "timecode.h"

namespace timecode_in {

namespace {

timecode::Decoder dec_;
bool              enabled_ = false;

// Pump buffer: 256 stereo int16 frames = 1024 bytes, ~5.8 ms at 44.1kHz.
// Small enough that tick() returns fast; large enough that the I²S DMA
// isn't hammered on every loop iteration.
constexpr int PUMP_FRAMES = 256;
int16_t       rxBuf_[PUMP_FRAMES * 2];

// Per-window diagnostics, cleared by takeStats().
int16_t  statsPeak_   = 0;
uint32_t statsFrames_ = 0;

} // namespace

void begin() {
    // Default to vinyl — that's what the hardware target (turntable into
    // line-in) will feed us. Control CD uses a different LFSR seed/taps,
    // so bit-lock can't happen cross-format even though ZC-based speed
    // estimation works on either carrier.
    dec_.begin(cfg::SAMPLE_RATE, timecode::Format::SeratoControlVinyl);
    // ES8388 on A1S V2.3 delivers I²S frames with channels swapped
    // relative to xwax's primary=R convention — SWITCH_PRIMARY puts the
    // decoder's primary/secondary assignment back in phase with Serato's
    // forward direction. Found by bring-up test with real vinyl + deck.
    dec_.setFlags(timecode::SWITCH_PRIMARY);
}

void setEnabled(bool on) { enabled_ = on; }
bool enabled()           { return enabled_; }

void tick() {
    if (!enabled_) return;

    // AudioBoardStream inherits Stream::available() / readBytes(). Drain
    // everything queued, not just one PUMP_FRAMES slice — with the audio
    // pipeline active the main loop can take >6 ms per iteration, so a
    // single 256-frame drain per call leaves the I²S RX ring overflowing
    // and the decoder sees discontinuous audio (won't lock). Safety cap
    // of 8 iterations keeps any one tick() bounded (~46 ms of audio).
    auto& k = codec::kit();
    constexpr int bytesPerFrame = 2 /*ch*/ * sizeof(int16_t);
    constexpr int maxBytes      = PUMP_FRAMES * bytesPerFrame;

    for (int iter = 0; iter < 8; ++iter) {
        int avail = k.available();
        if (avail < bytesPerFrame) break;
        int want = avail < maxBytes ? avail : maxBytes;
        want -= want % bytesPerFrame;
        if (want <= 0) break;

        int got = k.readBytes((uint8_t*)rxBuf_, want);
        int frames = got / bytesPerFrame;
        if (frames <= 0) break;

        // Scan for peak before handing off to the decoder — it doesn't
        // track amplitude and we don't want a second buffer pass
        // elsewhere. INT16_MIN's |.| overflows int16_t, so clamp it.
        for (int i = 0; i < frames * 2; ++i) {
            int16_t s = rxBuf_[i];
            int16_t a = (s == INT16_MIN) ? INT16_MAX : (int16_t)(s < 0 ? -s : s);
            if (a > statsPeak_) statsPeak_ = a;
        }
        statsFrames_ += (uint32_t)frames;
        dec_.pushFrames(rxBuf_, frames);

        // If the codec gave us less than we asked for, the ring is empty
        // — no point looping again this tick.
        if (got < want) break;
    }
}

float   speed()    { return dec_.speed(); }
bool    locked()   { return dec_.locked(); }
int32_t position() { return dec_.position(); }

Stats   takeStats() {
    Stats s{ statsPeak_, statsFrames_ };
    statsPeak_   = 0;
    statsFrames_ = 0;
    return s;
}

} // namespace timecode_in
