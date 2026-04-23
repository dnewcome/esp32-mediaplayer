#include "timecode_in.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "codec.h"
#include "config.h"
#include "timecode.h"

namespace timecode_in {

namespace {

timecode::Decoder dec_;
volatile bool     enabled_ = false;
// Track the format we configured the decoder with (the Decoder class
// doesn't expose a getter). Used by cycleFormat() to pick the next one
// and by isCdFormat() so the main-loop status line can report it.
timecode::Format  fmt_     = timecode::Format::SeratoControlVinyl;

// Pump buffer: 256 stereo int16 frames = 1024 bytes, ~5.8 ms at 44.1kHz.
constexpr int PUMP_FRAMES = 256;
int16_t       rxBuf_[PUMP_FRAMES * 2];

// --- Local-loop diagnostic source ---
//
// Source gating: main thread sets `wantLocalLoop_`; the task opens and
// closes the file. Doing the I/O ownership in the task avoids a race
// on the File handle (the task was already the only reader of rxBuf_).
// File-vs-ADC selection in the task reads `wantLocalLoop_` directly.
volatile bool wantLocalLoop_ = false;
bool          inLocalLoop_   = false;   // task-local; tracks whether file is open
File          loopFile_;
uint32_t      loopDataStart_ = 44;      // byte offset of PCM data in WAV

// Open timecode.wav and locate the PCM data start by searching the
// first 256 bytes for the "data" chunk marker. Standard 44-byte RIFF
// headers are the common case but extended fmt chunks or LIST tags
// push the real data offset further.
bool openLoopFile() {
    loopFile_ = SD_MMC.open("/timecode.wav", FILE_READ);
    if (!loopFile_) return false;
    uint8_t hdr[256];
    int n = loopFile_.read(hdr, sizeof(hdr));
    if (n < 44) { loopFile_.close(); return false; }
    loopDataStart_ = 44;
    for (int i = 0; i < n - 8; ++i) {
        if (hdr[i]=='d' && hdr[i+1]=='a' && hdr[i+2]=='t' && hdr[i+3]=='a') {
            loopDataStart_ = (uint32_t)(i + 8);
            break;
        }
    }
    loopFile_.seek(loopDataStart_);
    return true;
}

// Per-window diagnostics. Written by the task, read+reset by the main
// loop via takeStats(). Guarded by a portMUX spinlock — the naive
// volatile approach lets the task's read-modify-write clobber
// takeStats()'s reset, carrying counters (peak and frames) into the
// next window. Visible as frames bouncing well away from the expected
// 44k/sec and peak stuck above its real value.
int16_t      statsPeak_   = 0;
uint32_t     statsFrames_ = 0;
portMUX_TYPE statsMux_    = portMUX_INITIALIZER_UNLOCKED;

TaskHandle_t      tcTask_ = nullptr;

// Task body: continuously drains the codec RX ring (or timecode.wav in
// local-loop mode) and feeds the decoder. Lives on core 0 so it runs
// in parallel with the Arduino main loop (and player audio pipeline)
// on core 1 — before this was a task, tick() in loop() starved I²S TX
// enough to distort playback.
void taskEntry(void*) {
    auto& k = codec::kit();
    constexpr int bytesPerFrame = 2 /*ch*/ * sizeof(int16_t);
    constexpr int maxBytes      = PUMP_FRAMES * bytesPerFrame;
    // Micros-precise pacing for local-loop: file reads are near-instant
    // but the decoder expects ~SR samples per second. Without pacing we'd
    // blast the decoder at multi-MB/sec and its ZC timing would report
    // insane speed values.
    uint32_t loopNextUs = 0;

    for (;;) {
        // Source state machine: open/close the local-loop file as the
        // request flag changes. Only the task touches loopFile_.
        if (wantLocalLoop_ && !inLocalLoop_) {
            if (openLoopFile()) {
                inLocalLoop_ = true;
                loopNextUs   = micros();
                Serial.println(F("[tc] local-loop: /timecode.wav opened"));
            } else {
                wantLocalLoop_ = false;
                Serial.println(F("[tc] local-loop: open FAILED, staying on ADC"));
            }
        } else if (!wantLocalLoop_ && inLocalLoop_) {
            loopFile_.close();
            inLocalLoop_ = false;
            Serial.println(F("[tc] local-loop: closed"));
        }

        if (!enabled_) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int frames = 0;

        if (inLocalLoop_) {
            // Pace to real-time: wait until the scheduled micros for this batch.
            uint32_t now = micros();
            if ((int32_t)(loopNextUs - now) > 1000) {
                vTaskDelay(pdMS_TO_TICKS((loopNextUs - now) / 1000));
                continue;  // re-check wantLocalLoop_ and enabled_ after sleep
            }
            size_t got = loopFile_.read((uint8_t*)rxBuf_, maxBytes);
            if (got < (size_t)bytesPerFrame) {
                // EOF — loop back to PCM data start.
                loopFile_.seek(loopDataStart_);
                got = loopFile_.read((uint8_t*)rxBuf_, maxBytes);
            }
            frames = (int)(got / bytesPerFrame);
            if (frames <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
            loopNextUs += (uint32_t)frames * 1000000u / (uint32_t)cfg::SAMPLE_RATE;
        } else {
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
            frames = got / bytesPerFrame;
            if (frames <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        }

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
    // estimation works on either carrier. Runtime-switchable via
    // cycleFormat() for local-loop testing with /timecode.wav.
    dec_.begin(cfg::SAMPLE_RATE, fmt_);
    // Build the position LUT on the caller's thread so the ~2.5 s PSRAM
    // fill doesn't stall the tc task later. Lazy build inside pushFrames
    // would accumulate a burst in the codec FIFO or the local-loop file
    // read path — easier to just pay the cost up front.
    timecode::prebuildLut(fmt_);
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

void cycleFormat() {
    fmt_ = (fmt_ == timecode::Format::SeratoControlVinyl)
             ? timecode::Format::SeratoControlCD
             : timecode::Format::SeratoControlVinyl;
    // Overwrite the existing LUT buffer in place — avoids needing a
    // second 3 MB PSRAM chunk on the 4 MB board. Suspend the task
    // during the overwrite so no concurrent lookup reads half-written
    // entries. dec_.begin() inside the window resets decoder state,
    // so any transient is discarded anyway.
    if (tcTask_) vTaskSuspend(tcTask_);
    timecode::rebuildLutInPlace(fmt_);
    dec_.begin(cfg::SAMPLE_RATE, fmt_);
    // Different flag defaults per format, matching the typical source:
    //   Vinyl: turntable → ES8388 LINE2, channels swapped by the codec
    //          → SWITCH_PRIMARY restores Serato's primary=R convention.
    //   CD:    file-fed via local-loop, L/R land as-recorded
    //          → no flags; 0x0 achieves lock on /timecode.wav.
    // Empirically determined via tools/tc_flags_scan.py; users with a
    // different source combo (e.g. CD player into ES8388) can press `f`
    // to cycle flags manually.
    uint32_t flags = (fmt_ == timecode::Format::SeratoControlVinyl)
                     ? timecode::SWITCH_PRIMARY
                     : 0u;
    dec_.setFlags(flags);
    if (tcTask_) vTaskResume(tcTask_);
}

bool isCdFormat() {
    return fmt_ == timecode::Format::SeratoControlCD;
}

int      resolutionHz()    { return dec_.resolutionHz(); }
uint32_t totalDurationMs() { return timecode::totalDurationMs(fmt_); }

bool setLocalLoop(bool on) {
    wantLocalLoop_ = on;
    // Actual open/close + success confirmation happens in the task loop;
    // returning here only indicates "request accepted." Caller watches
    // Serial for the confirmation line or polls localLoop().
    return true;
}

bool localLoop() { return wantLocalLoop_; }

} // namespace timecode_in
