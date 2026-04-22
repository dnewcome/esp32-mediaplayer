#include "player.h"
#include "codec.h"
#include "config.h"
#include "wsola.h"

#include <SD_MMC.h>

// arduino-audio-tools — APIs can shift between versions. The include paths
// below target the current layout (as of the pschatzmann/arduino-audio-tools
// main branch). If these paths break after a lib update, check the examples
// under arduino-audio-tools/examples/examples-player/ for the current names.
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"

namespace player {

namespace {

// The AudioBoardStream lives in `codec` — shared with timecode_in so the
// RX path can capture line-in simultaneously.
inline AudioBoardStream& kit() { return codec::kit(); }

MP3DecoderHelix mp3;
WAVDecoder      wav;

// ----- Audio chain -----
//
//   Pitched mode (varispeed, like a record turntable — pitch tracks speed):
//     SD file --(copier)--> EncodedAudioStream --> kit(I2S+codec)
//     Speed changes the I2S clock: kit.sample_rate = native_rate * speed.
//     We tried ResampleStream here first, but its audioInfoOut() broadcasts
//     `input_rate * step_size` downstream, which reclocks the I2S codec —
//     a sample-rate converter, not a varispeed tool. Running the I2S clock
//     directly is simpler and matches the platter-speed mental model.
//
//   Keylock mode (time stretch, pitch preserved):
//     SD file --(copier)--> EncodedAudioStream --> WsolaStream --> kit
//     kit stays at the file's native rate; WSOLA does the stretching.

wsola::WsolaStream wsolaStage(kit());   // audio-tools AudioStream wrapper (see wsola.h)

// Two encoded streams; we point the copier at whichever matches the file type.
EncodedAudioStream mp3ToKit (&kit(),      &mp3);
EncodedAudioStream wavToKit (&kit(),      &wav);
EncodedAudioStream mp3ToWsola(&wsolaStage, &mp3);
EncodedAudioStream wavToWsola(&wsolaStage, &wav);

StreamCopy copier;

File audioFile;
bool  playing   = false;
bool  paused    = false;
float curSpeed  = 1.0f;
Mode  curMode   = Mode::Pitched;

// Native (un-pitched) sample rate of the current track. 0 until the decoder
// has reported; setSpeed() multiplies by this to set the I2S clock.
uint32_t nativeRate_ = 0;

uint32_t cuePos_    = 0;
bool     cueValid_  = false;

enum class Fmt { Unknown, MP3, WAV };

Fmt detectFmt(const char* path) {
    int n = strlen(path);
    if (n < 5) return Fmt::Unknown;
    const char* e = path + n - 4;
    if (strcasecmp(e, ".mp3") == 0) return Fmt::MP3;
    if (strcasecmp(e, ".wav") == 0) return Fmt::WAV;
    return Fmt::Unknown;
}

AudioStream* pickSink(Fmt fmt) {
    const bool keylock = (curMode == Mode::Keylock);
    switch (fmt) {
        case Fmt::MP3: return keylock ? (AudioStream*)&mp3ToWsola : (AudioStream*)&mp3ToKit;
        case Fmt::WAV: return keylock ? (AudioStream*)&wavToWsola : (AudioStream*)&wavToKit;
        default:       return nullptr;
    }
}

// Latch the decoder's reported rate the first time kit has a non-zero rate after
// a track starts. The decoder notifies asynchronously on the first decoded frame.
void maybeLatchNativeRate() {
    if (nativeRate_ != 0) return;
    uint32_t r = kit().audioInfo().sample_rate;
    if (r != 0) nativeRate_ = r;
}

void applyPitchedSpeed() {
    if (nativeRate_ == 0) return;  // nothing to clock against yet
    AudioInfo info = kit().audioInfo();
    info.sample_rate = (uint32_t)(nativeRate_ * curSpeed);
    kit().setAudioInfo(info);
}

void applySpeedToActiveStage() {
    if (curMode == Mode::Keylock) {
        wsolaStage.setSpeed(curSpeed);
    } else {
        applyPitchedSpeed();
    }
}

} // namespace

void begin() {
    // Codec init lives in codec::begin() (shared with timecode_in).
    codec::begin();

    auto cfgOut = kit().defaultConfig(RXTX_MODE);
    cfgOut.sample_rate     = cfg::SAMPLE_RATE;
    cfgOut.channels        = cfg::CHANNELS;
    cfgOut.bits_per_sample = cfg::BITS_PER_SAMPLE;

    wsolaStage.begin(cfg::SAMPLE_RATE, cfg::CHANNELS);

    auto cfgEnc = mp3ToKit.defaultConfig();
    cfgEnc.copyFrom(cfgOut);
    mp3ToKit  .begin(cfgEnc);
    wavToKit  .begin(cfgEnc);
    mp3ToWsola.begin(cfgEnc);
    wavToWsola.begin(cfgEnc);
}

void loopTick() {
    if (!playing || paused) return;
    size_t copied = copier.copy();
    // After the first decoded frame the codec's sample rate reflects the
    // file; latch it and re-apply any pending speed change.
    if (nativeRate_ == 0) {
        maybeLatchNativeRate();
        if (nativeRate_ != 0 && curMode == Mode::Pitched && curSpeed != 1.0f) {
            applyPitchedSpeed();
        }
    }
    if (copied == 0 && !audioFile.available()) {
        stop();
    }
}

bool play(const char* absolutePath) {
    stop();
    Fmt fmt = detectFmt(absolutePath);
    AudioStream* sink = pickSink(fmt);
    if (!sink) return false;

    audioFile = SD_MMC.open(absolutePath);
    if (!audioFile) return false;

    // nativeRate_ gets re-learned from the decoder's notification on the first
    // frame. Until then we can't apply pitched speed — loopTick() latches it.
    nativeRate_ = 0;
    copier.begin(*sink, audioFile);
    // Apply current speed to the active stage. For Keylock this takes effect
    // immediately (WSOLA speed is independent of sample rate). For Pitched it
    // no-ops here (nativeRate_ is still 0) and gets re-applied by loopTick()
    // as soon as the decoder reports.
    applySpeedToActiveStage();
    playing = true;
    paused  = false;
    return true;
}

void togglePause() {
    if (!playing) return;
    paused = !paused;
    kit().setMute(paused);
}

void stop() {
    playing = false;
    paused  = false;
    cueValid_ = false;
    copier.end();
    if (audioFile) audioFile.close();
    kit().setMute(false);
    wsolaStage.reset();
}

void setCuePoint() {
    if (!audioFile) return;
    cuePos_   = audioFile.position();
    cueValid_ = true;
}

void jumpToCue() {
    if (!cueValid_ || !audioFile) return;
    audioFile.seek(cuePos_);
    // Reset downstream DSP state so stale OLA/pitch state doesn't bleed.
    wsolaStage.reset();
}

bool hasCue() { return cueValid_; }

void setSpeed(float s) {
    if (s < cfg::SPEED_MIN) s = cfg::SPEED_MIN;
    if (s > cfg::SPEED_MAX) s = cfg::SPEED_MAX;
    curSpeed = s;
    applySpeedToActiveStage();
}

void setMode(Mode m) {
    if (m == curMode) return;
    curMode = m;
    // Changing mode mid-track would need the copier to re-bind to the new
    // sink; simplest policy is: mode takes effect on the next play().
}

Mode  mode()      { return curMode; }
float speed()     { return curSpeed; }
bool  isPlaying() { return playing;  }
bool  isPaused()  { return paused;   }

uint32_t filePosition() {
    return audioFile ? audioFile.position() : 0;
}

} // namespace player
