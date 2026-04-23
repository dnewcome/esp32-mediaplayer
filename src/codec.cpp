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

// In-chain volume meter — the arduino-audio-tools library primitive for
// tapping peak levels. Constructed lazily in txSink() so it can reference
// kit() safely across translation-unit init ordering.
VolumeMeter* txMeter_ = nullptr;

// Track current output volume (0..100) at file scope so begin() and
// adjustOutputVolume() agree. Starts at DEFAULT_VOLUME so the first `v`/
// `V` press doesn't jump from a stale 100.
//
// Why not 100: the ES8388's DAC output amps crosstalk onto the ADC
// LINE IN at roughly unity gain by the time PGA=+24dB is applied.
// Running the DAC at ~50% keeps the crosstalk below the typical
// turntable timecode level (~21k peak on vinyl at 33⅓), which is what
// lets pitch tracking work at all. Turn up with `V` if you don't need
// concurrent timecode decoding; turn up externally on the DJ mixer if
// you do.
constexpr int DEFAULT_VOLUME = 50;
int outputVol_ = DEFAULT_VOLUME;

// Input PGA default. Library default is 100 (+24 dB), which rails the
// ADC on a live turntable cartridge feeding LINE2 on Dan's bring-up
// hardware. 0 (~0 dB) lets the line-level signal sit at rx ~1000 —
// well above the coupling floor and far from clipping — which is what
// finally let the decoder lock. If your source is quieter (mic, RIAA
// preamp output), bump with G.
constexpr int DEFAULT_INPUT_GAIN = 0;
int inputGain_ = DEFAULT_INPUT_GAIN;
} // namespace

AudioStream& txSink() {
    // Lazy: kit() is a function-local static, guaranteed constructed on
    // first call. Instantiate the VolumeMeter the same way so we can't
    // hit cross-TU static init ordering bugs.
    static VolumeMeter inst(kit());
    txMeter_ = &inst;
    return inst;
}

int16_t takeTxPeak() {
    if (!txMeter_) return 0;
    // VolumeMeter::volume() returns the current peak amplitude. It's an
    // EMA-style running value so we don't need reset-on-read — it'll
    // decay naturally as quieter audio flows through.
    float v = txMeter_->volume();
    if (v < 0.0f)        v = 0.0f;
    if (v > (float)INT16_MAX) v = (float)INT16_MAX;
    return (int16_t)v;
}

uint32_t takeTxWriteCount() {
    // Library handles this internally — stub retained for API stability.
    return 0;
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

    // Clamp default DAC volume so DAC→ADC crosstalk stays below
    // turntable timecode level (see DEFAULT_VOLUME comment above).
    k.setVolume((float)outputVol_ / 100.0f);

    // Set input PGA to 0 dB for turntable line-level signal — see
    // DEFAULT_INPUT_GAIN comment above. G to boost at runtime.
    k.setInputVolume(inputGain_);

    if (txMeter_) txMeter_->begin();
}

void adjustOutputVolume(int delta) {
    // AudioBoardStream::setVolume expects a float in 0.0..1.0; it both
    // scales output samples in software AND writes a codec register.
    // Passing an int 0..100 as-is gets cast to float — a "10" becomes
    // a 10× gain, clipping every sample, which reads as "still max
    // loud" until the int hits 0 and it mutes. Track 0..100 for UX but
    // scale to 0.0..1.0 at the boundary.
    outputVol_ += delta;
    if (outputVol_ < 0)   outputVol_ = 0;
    if (outputVol_ > 100) outputVol_ = 100;
    kit().setVolume((float)outputVol_ / 100.0f);
    Serial.print(F("output volume: "));
    Serial.print(outputVol_);
    Serial.println(F("/100"));
}

void adjustInputGain(int delta) {
    // setInputVolume takes 0..100, mapped internally to MIC_GAIN_0DB..24DB in
    // 3 dB steps. File-scope counter stays in sync with what begin()
    // installed, so the first `g`/`G` press moves from the real state.
    inputGain_ += delta;
    if (inputGain_ < 0)   inputGain_ = 0;
    if (inputGain_ > 100) inputGain_ = 100;
    kit().setInputVolume(inputGain_);
    Serial.print(F("input gain: "));
    Serial.print(inputGain_);
    Serial.print(F("/100  (~"));
    Serial.print((inputGain_ * 24) / 100);
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
