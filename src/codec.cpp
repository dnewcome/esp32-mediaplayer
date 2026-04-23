#include "codec.h"
#include "config.h"

#include <Arduino.h>
#include <Wire.h>

namespace codec {

namespace {
bool started_ = false;

// ES8388 I²C slave address on A1S V2.3 boards. (Named I2C_ADDR rather
// than ES8388_ADDR — the library header #defines that symbol.)
constexpr uint8_t I2C_ADDR = 0x10;

// Peak of TX samples written through txSink, written by the main-loop
// copier (core 1) and read+reset by the [tc] trace print (also core 1,
// same thread — no cross-core races to worry about for this one).
int16_t txPeak_ = 0;

// Wrapper around kit() that scans each PCM buffer for peak amplitude
// before forwarding the write. int16 stereo format is assumed (matches
// cfg::BITS_PER_SAMPLE = 16, cfg::CHANNELS = 2). Everything that's not
// write() just forwards to kit so the MP3/WAV decoder chain behaves
// identically — including setAudioInfo notifications that reclock I²S
// for varispeed.
class TxSink : public AudioStream {
public:
    void bind(AudioBoardStream& target) { target_ = &target; }

    size_t write(uint8_t c) override {
        // I2SCodecStream hides single-byte write; forward as a 1-byte buf.
        return target_ ? target_->write(&c, 1) : 0;
    }
    size_t write(const uint8_t* data, size_t len) override {
        if (!target_) return 0;
        // Peak across the buffer. Decoded PCM is int16 stereo.
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        const size_t   n = len / sizeof(int16_t);
        int16_t p = 0;
        for (size_t i = 0; i < n; ++i) {
            int16_t v = s[i];
            int16_t a = (v == INT16_MIN) ? INT16_MAX
                                         : (int16_t)(v < 0 ? -v : v);
            if (a > p) p = a;
        }
        if (p > txPeak_) txPeak_ = p;
        txWriteCount_++;
        txWriteCountTotal_++;

        // Throttled trace so we can see with our own eyes whether this path
        // is ever hit. Prints at most once every 500 ms.
        static uint32_t lastDbg = 0;
        uint32_t now = millis();
        if (now - lastDbg >= 500) {
            lastDbg = now;
            Serial.printf("[tx-hit] len=%u p=%d total=%u\n",
                          (unsigned)len, (int)p, (unsigned)txWriteCountTotal_);
        }
        return target_->write(data, len);
    }

    uint32_t txWriteCount_      = 0;
    uint32_t txWriteCountTotal_ = 0;

    int available() override          { return target_ ? target_->available() : 0; }
    int availableForWrite() override  { return target_ ? target_->availableForWrite() : 0; }
    size_t readBytes(uint8_t* b, size_t n) override {
        return target_ ? target_->readBytes(b, n) : 0;
    }
    AudioInfo audioInfo() override {
        return target_ ? target_->audioInfo() : AudioInfo{};
    }
    void setAudioInfo(AudioInfo info) override {
        AudioStream::setAudioInfo(info);
        if (target_) target_->setAudioInfo(info);
    }

private:
    AudioBoardStream* target_ = nullptr;
};

TxSink txSink_;
} // namespace

AudioStream& txSink() { return txSink_; }

int16_t takeTxPeak() {
    int16_t p = txPeak_;
    txPeak_ = 0;
    return p;
}

uint32_t takeTxWriteCount() {
    uint32_t c = txSink_.txWriteCount_;
    txSink_.txWriteCount_ = 0;
    return c;
}

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
    txSink_.bind(k);
    auto cfg = k.defaultConfig(RXTX_MODE);
    cfg.sample_rate     = cfg::SAMPLE_RATE;
    cfg.channels        = cfg::CHANNELS;
    cfg.bits_per_sample = cfg::BITS_PER_SAMPLE;
    // Route the ADC to LINPUT2/RINPUT2 — the external 3.5mm aux-in jack on
    // A1S V2.3 boards. The library default (ADC_INPUT_LINE1) is the
    // on-board differential mic pair, which ignores the jack entirely.
    cfg.input_device    = ADC_INPUT_LINE2;
    k.begin(cfg);
    started_ = true;

    // Drop the ADC input PGA to 0 dB. Library default (ES8388_DEFAULT_INPUT_GAIN
    // = 25, ~+24 dB) is sized for mic-level sources and rails the ADC on a
    // line-level signal (turntable line-out, phono-preamp output, mixer send).
    // Adjustable live via adjustInputGain() — start at 0 and bump up if the
    // source is too quiet.
    k.setInputVolume(0);
}

void adjustOutputVolume(int delta) {
    // AudioBoardStream::setVolume expects a float in 0.0..1.0; it both
    // scales output samples in software AND writes a codec register.
    // Passing an int 0..100 as-is gets cast to float — a "10" becomes
    // a 10× gain, clipping every sample, which reads as "still max
    // loud" until the int hits 0 and it mutes. Track 0..100 for UX but
    // scale to 0.0..1.0 at the boundary.
    static int vol = 100;
    vol += delta;
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    kit().setVolume((float)vol / 100.0f);
    Serial.print(F("output volume: "));
    Serial.print(vol);
    Serial.println(F("/100"));
}

void adjustInputGain(int delta) {
    // setInputVolume takes 0..100, mapped internally to MIC_GAIN_0DB..24DB in
    // 3 dB steps. Track our own value since the library doesn't expose a
    // getter. Clamped to the same 0..100 range.
    static int vol = 0;
    vol += delta;
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    kit().setInputVolume(vol);
    Serial.print(F("input gain: "));
    Serial.print(vol);
    Serial.print(F("/100  (~"));
    Serial.print((vol * 24) / 100);
    Serial.println(F(" dB)"));
}

uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    if (Wire.requestFrom((int)I2C_ADDR, 1) != 1) return 0xFF;
    return (uint8_t)Wire.read();
}

void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void dumpRegs() {
    // ES8388 has 53 documented control registers (0x00..0x34). Print in
    // rows of 8 with the address offset leading each row — matches the
    // datasheet's register table layout so it's easy to look up bits.
    Serial.println(F("\n[es8388 regs]"));
    for (uint8_t base = 0; base <= 0x34; base += 8) {
        Serial.printf("  %02X:", base);
        for (uint8_t i = 0; i < 8 && (base + i) <= 0x34; ++i) {
            Serial.printf(" %02X", readReg(base + i));
        }
        Serial.println();
    }
}

} // namespace codec
