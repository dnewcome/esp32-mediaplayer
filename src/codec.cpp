#include "codec.h"
#include "config.h"

namespace codec {

namespace {
bool started_ = false;
} // namespace

AudioBoardStream& kit() {
    // Function-local static avoids cross-TU static-init-order issues:
    // player.cpp's globals capture this reference at their own init time.
#if defined(MEDIAPLAYER_CODEC_AC101)
    static AudioBoardStream instance(AudioKitAC101);
#else
    static AudioBoardStream instance(AudioKitEs8388V1);
#endif
    return instance;
}

void begin() {
    if (started_) return;
    auto& k = kit();
    auto cfg = k.defaultConfig(RXTX_MODE);
    cfg.sample_rate     = cfg::SAMPLE_RATE;
    cfg.channels        = cfg::CHANNELS;
    cfg.bits_per_sample = cfg::BITS_PER_SAMPLE;
    k.begin(cfg);
    started_ = true;
}

} // namespace codec
