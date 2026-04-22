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
//   SD file --(copier)--> encoded stream --> [speed stage] --> kit(I2S+codec)
//
// [speed stage] is one of:
//   - ResampleStream (pitched; pitch tracks speed, like a varispeed turntable)
//   - WSOLA         (keylock; time stretched, pitch preserved)
//
// For Phase 1 both are wired in parallel; which one drives the codec is chosen
// by setMode(). Whichever is inactive is bypassed (its output isn't consumed).

ResampleStream resampler(kit());
wsola::WsolaStream wsolaStage(kit());   // audio-tools AudioStream wrapper (see wsola.h)

// Two encoded streams; we point the copier at whichever matches the file type.
EncodedAudioStream mp3ToResample(&resampler, &mp3);
EncodedAudioStream wavToResample(&resampler, &wav);
EncodedAudioStream mp3ToWsola   (&wsolaStage, &mp3);
EncodedAudioStream wavToWsola   (&wsolaStage, &wav);

StreamCopy copier;

File audioFile;
bool  playing   = false;
bool  paused    = false;
float curSpeed  = 1.0f;
Mode  curMode   = Mode::Pitched;

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
        case Fmt::MP3: return keylock ? (AudioStream*)&mp3ToWsola : (AudioStream*)&mp3ToResample;
        case Fmt::WAV: return keylock ? (AudioStream*)&wavToWsola : (AudioStream*)&wavToResample;
        default:       return nullptr;
    }
}

void applySpeedToActiveStage() {
    if (curMode == Mode::Keylock) {
        wsolaStage.setSpeed(curSpeed);
        resampler.setStepSize(1.0f);
    } else {
        resampler.setStepSize(curSpeed);
        wsolaStage.setSpeed(1.0f);
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

    auto cfgRs = resampler.defaultConfig();
    cfgRs.copyFrom(cfgOut);
    cfgRs.step_size = 1.0f;
    resampler.begin(cfgRs);

    wsolaStage.begin(cfg::SAMPLE_RATE, cfg::CHANNELS);

    auto cfgEnc = mp3ToResample.defaultConfig();
    cfgEnc.copyFrom(cfgOut);
    mp3ToResample.begin(cfgEnc);
    wavToResample.begin(cfgEnc);
    mp3ToWsola   .begin(cfgEnc);
    wavToWsola   .begin(cfgEnc);
}

void loopTick() {
    if (!playing || paused) return;
    size_t copied = copier.copy();
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

    applySpeedToActiveStage();
    copier.begin(*sink, audioFile);
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
