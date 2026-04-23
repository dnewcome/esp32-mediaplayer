#include "timecode_in.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "codec.h"
#include "config.h"
#include "timecode.h"

namespace timecode_in {

namespace {

timecode::Decoder dec_;
volatile bool     enabled_ = false;

// Pump buffer: 256 stereo int16 frames = 1024 bytes, ~5.8 ms at 44.1kHz.
constexpr int PUMP_FRAMES = 256;
int16_t       rxBuf_[PUMP_FRAMES * 2];

// Per-window diagnostics. Written by the task, read+reset by the main
// loop via takeStats(). Guarded by a portMUX spinlock — the naive
// volatile + read-modify-write approach lets the task clobber
// takeStats()'s reset, which carries the old peak into the next
// window. Under a clipping carrier the result was "peak pinned at
// 32767 forever" even after the real signal normalised.
int16_t         statsPeak_   = 0;
uint32_t        statsFrames_ = 0;
portMUX_TYPE    statsMux_    = portMUX_INITIALIZER_UNLOCKED;

TaskHandle_t      tcTask_ = nullptr;

// Task body: continuously drains the codec RX ring and feeds the
// decoder. Lives on core 0 so it runs in parallel with the Arduino
// main loop (and player audio pipeline) on core 1 — before this was a
// task, tick() in loop() starved I²S TX enough to distort playback.
void taskEntry(void*) {
    auto& k = codec::kit();
    constexpr int bytesPerFrame = 2 /*ch*/ * sizeof(int16_t);
    constexpr int maxBytes      = PUMP_FRAMES * bytesPerFrame;

    for (;;) {
        if (!enabled_) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int avail = k.available();
        if (avail < bytesPerFrame) {
            // Sleep short enough that the RX DMA ring can't overflow
            // between polls, but long enough to yield real cycles back
            // to core 0's idle/watchdog tasks.
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        int want = avail < maxBytes ? avail : maxBytes;
        want -= want % bytesPerFrame;
        if (want <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

        int got = k.readBytes((uint8_t*)rxBuf_, want);
        int frames = got / bytesPerFrame;
        if (frames <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

        // Scan for this batch's peak (not accumulated across batches —
        // cross-batch accumulation belongs inside the critical section,
        // otherwise a takeStats() reset between read and write gets
        // clobbered). INT16_MIN's |.| overflows int16_t, so clamp it.
        int16_t batchPeak = 0;
        for (int i = 0; i < frames * 2; ++i) {
            int16_t s = rxBuf_[i];
            int16_t a = (s == INT16_MIN) ? INT16_MAX : (int16_t)(s < 0 ? -s : s);
            if (a > batchPeak) batchPeak = a;
        }

        taskENTER_CRITICAL(&statsMux_);
        if (batchPeak > statsPeak_) statsPeak_ = batchPeak;
        statsFrames_ += (uint32_t)frames;
        taskEXIT_CRITICAL(&statsMux_);

        dec_.pushFrames(rxBuf_, frames);
    }
}

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

    // Pin to core 0. Arduino's loop() (and the audio copier it drives)
    // runs on core 1, so this keeps decode CPU off the audio path.
    // Priority 5 is higher than loopTask's default 1 but lower than the
    // I²S/Wi-Fi service tasks — enough to drain promptly without
    // preempting critical-path work.
    xTaskCreatePinnedToCore(taskEntry, "tcin", 4096, nullptr, 5, &tcTask_, 0);
}

void setEnabled(bool on) { enabled_ = on; }
bool enabled()           { return enabled_; }

void tick() {
    // Drain now happens in taskEntry() on core 0. Kept as a no-op so
    // the existing main-loop call site stays valid.
}

float   speed()    { return dec_.speed(); }
bool    locked()   { return dec_.locked(); }
int32_t position() { return dec_.position(); }

Stats   takeStats() {
    taskENTER_CRITICAL(&statsMux_);
    Stats s{ statsPeak_, statsFrames_ };
    statsPeak_   = 0;
    statsFrames_ = 0;
    taskEXIT_CRITICAL(&statsMux_);
    return s;
}

uint32_t cycleFlags() {
    uint32_t f = (dec_.flags() + 1u) & 0x7u;
    dec_.setFlags(f);
    return f;
}

} // namespace timecode_in
