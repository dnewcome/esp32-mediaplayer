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
// RX path can capture line-in simultaneously. For the TX write side we
// use codec::txSink(), a pass-through wrapper that logs PCM peak to
// diagnose DAC-vs-ADC coupling. RX drain and I²S reclocks still go
// through kit() directly.
inline AudioBoardStream& kit() { return codec::kit(); }
inline AudioStream&      out() { return codec::txSink(); }

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

wsola::WsolaStream wsolaStage(kit());

// Two encoded streams; we point the copier at whichever matches the file type.
EncodedAudioStream mp3ToKit (&kit(),       &mp3);
EncodedAudioStream wavToKit (&kit(),       &wav);
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

// Track duration / seek math. Latched in play() from file-header inspection
// (not from the decoder, which reports async). 0 until determined.
uint32_t trackDurationMs_ = 0;
uint32_t dataStart_       = 0;      // byte offset of PCM data in file (WAV header / ID3v2)
uint16_t channels_        = 2;      // derived from file; most WAV/MP3 are stereo
uint16_t bitsPerSample_   = 16;     // WAV only; MP3 uses bitrate instead
uint32_t headerSR_        = 0;      // sample rate parsed from WAV fmt chunk (0 for MP3)
uint32_t mp3Bitrate_      = 0;      // bits per second; 0 for WAV / unparsed MP3

uint32_t cuePos_    = 0;
bool     cueValid_  = false;

enum class Fmt { Unknown, MP3, WAV };

// Standard MP3 bitrate table (kbps) — indexed by the 4-bit bitrate
// field in a frame header. MPEG-1 Layer III only (which is what MP3
// files actually contain). Index 0 = "free format" (unknown), 15 =
// reserved/invalid. All others are CBR values in kbps.
constexpr uint16_t kMp3Bitrates_Mpeg1Layer3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};

// Parse WAV header from the first `bufLen` bytes. On success, fills
// dataStart_, channels_, bitsPerSample_, headerSR_ and returns true.
// Handles non-44-byte headers (extended fmt chunks, LIST tags) by
// scanning for the 'data' chunk marker.
bool parseWavHeader(const uint8_t* buf, size_t bufLen) {
    if (bufLen < 44) return false;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return false;
    // fmt chunk starts at byte 12; chunk layout is {id[4], size[4], data[size]}.
    // channels at +10, sample_rate at +12, bits at +22 from fmt chunk start.
    // Most standard WAVs have fmt right after WAVE, but scan for it to be safe.
    size_t p = 12;
    while (p + 8 <= bufLen) {
        uint32_t sz = (uint32_t)buf[p+4] | ((uint32_t)buf[p+5] << 8)
                    | ((uint32_t)buf[p+6] << 16) | ((uint32_t)buf[p+7] << 24);
        if (memcmp(buf + p, "fmt ", 4) == 0 && p + 8 + 16 <= bufLen) {
            channels_      = (uint16_t)(buf[p+10] | (buf[p+11] << 8));
            headerSR_      = (uint32_t)(buf[p+12] | (buf[p+13] << 8)
                            | (buf[p+14] << 16) | (buf[p+15] << 24));
            bitsPerSample_ = (uint16_t)(buf[p+22] | (buf[p+23] << 8));
        } else if (memcmp(buf + p, "data", 4) == 0) {
            dataStart_ = (uint32_t)(p + 8);
            return headerSR_ > 0 && channels_ > 0;
        }
        p += 8 + sz;
        // Guard against malformed chunks (size 0 would loop forever, huge
        // size could wrap past bufLen — we just bail).
        if (sz == 0 || p > bufLen) break;
    }
    return false;
}

// Parse MP3 CBR bitrate from the first frame header found in `buf`.
// Skips an ID3v2 tag if present (starts with "ID3", next 6 bytes
// encode a synchsafe length). Finds the first byte pair where
// `byte0 == 0xFF && (byte1 & 0xE0) == 0xE0` — the 11-bit frame sync.
// On success fills dataStart_ and mp3Bitrate_ and returns true.
bool parseMp3Header(const uint8_t* buf, size_t bufLen) {
    size_t start = 0;
    if (bufLen >= 10 && buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        // Synchsafe 28-bit size (7 bits per byte, MSB clear).
        uint32_t sz = ((uint32_t)(buf[6] & 0x7F) << 21)
                    | ((uint32_t)(buf[7] & 0x7F) << 14)
                    | ((uint32_t)(buf[8] & 0x7F) << 7)
                    |  (uint32_t)(buf[9] & 0x7F);
        start = 10 + sz;
    }
    // First valid frame sync in [start, bufLen).
    for (size_t p = start; p + 4 <= bufLen; ++p) {
        if (buf[p] != 0xFF || (buf[p+1] & 0xE0) != 0xE0) continue;
        // Version bits[4:3], layer bits[2:1] — MPEG-1 Layer III = 11, 01.
        uint8_t version = (buf[p+1] >> 3) & 0x3;
        uint8_t layer   = (buf[p+1] >> 1) & 0x3;
        if (version != 0x3 || layer != 0x1) continue;   // not MPEG-1 L3, keep scanning
        uint8_t brIdx = (buf[p+2] >> 4) & 0xF;
        uint16_t brKbps = kMp3Bitrates_Mpeg1Layer3[brIdx];
        if (brKbps == 0) continue;   // free-format or reserved — keep scanning
        mp3Bitrate_ = (uint32_t)brKbps * 1000;  // bits per second
        dataStart_  = (uint32_t)start;          // audio data starts after ID3
        return true;
    }
    return false;
}

// Peek the first chunk of audioFile and fill track-duration fields.
// Called from play() after SD_MMC.open succeeds. File seeks back to 0
// before returning so the normal decoder path starts clean.
void latchTrackDuration(Fmt fmt) {
    trackDurationMs_ = 0;
    dataStart_       = 0;
    headerSR_        = 0;
    mp3Bitrate_      = 0;
    channels_        = 2;
    bitsPerSample_   = 16;

    uint8_t hdr[512];
    size_t n = audioFile.read(hdr, sizeof(hdr));
    audioFile.seek(0);  // decoder expects to read from the top

    uint32_t fileSize = (uint32_t)audioFile.size();

    if (fmt == Fmt::WAV && parseWavHeader(hdr, n)) {
        uint32_t frameBytes = (uint32_t)channels_ * (bitsPerSample_ / 8);
        if (frameBytes == 0) return;
        uint64_t pcmBytes = (fileSize > dataStart_) ? (uint64_t)(fileSize - dataStart_) : 0;
        trackDurationMs_ = (uint32_t)(pcmBytes * 1000 / (headerSR_ * frameBytes));
        Serial.printf("[play] WAV: %u Hz, %u ch, %u-bit, data@%u, %u ms\n",
                      (unsigned)headerSR_, (unsigned)channels_,
                      (unsigned)bitsPerSample_, (unsigned)dataStart_,
                      (unsigned)trackDurationMs_);
    } else if (fmt == Fmt::MP3 && parseMp3Header(hdr, n)) {
        uint64_t audioBytes = (fileSize > dataStart_) ? (uint64_t)(fileSize - dataStart_) : 0;
        trackDurationMs_ = (uint32_t)(audioBytes * 8 * 1000 / mp3Bitrate_);
        Serial.printf("[play] MP3: %u kbps, audio@%u, ~%u ms (CBR estimate)\n",
                      (unsigned)(mp3Bitrate_ / 1000), (unsigned)dataStart_,
                      (unsigned)trackDurationMs_);
    } else {
        Serial.println(F("[play] duration parse failed; trackDurationMs_ = 0"));
    }
}

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
    // Inspect the file header once to latch duration / PCM data offset
    // for positionMs() and seekToMs(). Seeks file back to 0 when done.
    latchTrackDuration(fmt);
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
    trackDurationMs_ = 0;
    dataStart_       = 0;
    headerSR_        = 0;
    mp3Bitrate_      = 0;
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

uint32_t trackDurationMs() { return trackDurationMs_; }

uint32_t positionMs() {
    if (!audioFile) return 0;
    uint32_t bytePos = audioFile.position();
    // bytePos can be < dataStart_ briefly right after play() when the
    // file handle is freshly opened and hasn't been stepped yet.
    uint32_t offset = bytePos > dataStart_ ? bytePos - dataStart_ : 0;
    if (headerSR_ > 0) {
        // WAV: bytes → samples → ms.
        uint32_t frameBytes = (uint32_t)channels_ * (bitsPerSample_ / 8);
        if (frameBytes == 0) return 0;
        return (uint32_t)((uint64_t)offset * 1000 / (headerSR_ * frameBytes));
    }
    if (mp3Bitrate_ > 0) {
        // MP3 CBR: bytes × 8 / bps × 1000. Wildly off on VBR content,
        // but good enough for proportional mapping and drift detection.
        return (uint32_t)((uint64_t)offset * 8 * 1000 / mp3Bitrate_);
    }
    return 0;
}

} // namespace player
