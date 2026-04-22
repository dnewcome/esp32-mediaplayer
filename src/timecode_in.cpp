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

} // namespace

void begin() {
    dec_.begin(cfg::SAMPLE_RATE, timecode::Format::SeratoControlCD);
}

void setEnabled(bool on) { enabled_ = on; }
bool enabled()           { return enabled_; }

void tick() {
    if (!enabled_) return;

    // AudioBoardStream inherits Stream::available() / readBytes(). We
    // only pull what's already queued, so tick() is non-blocking.
    auto& k = codec::kit();
    int avail = k.available();
    if (avail <= 0) return;

    constexpr int bytesPerFrame = 2 /*ch*/ * sizeof(int16_t);
    int maxBytes = PUMP_FRAMES * bytesPerFrame;
    int want     = avail < maxBytes ? avail : maxBytes;
    want -= want % bytesPerFrame;  // whole frames only
    if (want <= 0) return;

    int got = k.readBytes((uint8_t*)rxBuf_, want);
    int frames = got / bytesPerFrame;
    if (frames > 0) dec_.pushFrames(rxBuf_, frames);
}

float   speed()    { return dec_.speed(); }
bool    locked()   { return dec_.locked(); }
int32_t position() { return dec_.position(); }

} // namespace timecode_in
