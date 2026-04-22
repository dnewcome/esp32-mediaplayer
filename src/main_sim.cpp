// Wokwi headless-simulator entry point. Swapped in for src/main.cpp by the
// esp32-sim build env (see platformio.ini). Exercises the browser, controls,
// and OLED against a stubbed file list — no codec, no SD, no DSP. State
// transitions are traced to Serial so the scenario runner can assert.

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "controls.h"
#include "ui.h"

namespace {

const char* const kFiles[] = {
    "track01.mp3",
    "track02.mp3",
    "loop.wav",
};
constexpr int kFileCount = sizeof(kFiles) / sizeof(kFiles[0]);

enum class Screen { Browser, Playing };
Screen screen    = Screen::Browser;
int    selected  = 0;
float  simSpeed  = 1.0f;
bool   simPaused = false;
bool   simKey    = false;
bool   simCue    = false;

void trace() {
    if (screen == Screen::Browser) {
        Serial.printf("SIM: browser selected=%d name=%s\n",
                      selected, kFiles[selected]);
    } else {
        Serial.printf("SIM: playing name=%s speed=%.2f paused=%d keylock=%d cue=%d\n",
                      kFiles[selected], simSpeed, simPaused, simKey, simCue);
    }
}

void redraw() {
    if (screen == Screen::Browser) {
        ui::showBrowser(kFiles, kFileCount, selected);
    } else {
        ui::showNowPlaying(kFiles[selected], simSpeed, simPaused, simKey, simCue);
    }
    trace();
}

void handleBrowser(controls::Event e) {
    switch (e) {
        case controls::Event::EncoderCW:
            if (selected + 1 < kFileCount) ++selected;
            break;
        case controls::Event::EncoderCCW:
            if (selected > 0) --selected;
            break;
        case controls::Event::EncoderPress:
        case controls::Event::PlayPress:
            screen    = Screen::Playing;
            simSpeed  = 1.0f;
            simPaused = false;
            break;
        default: break;
    }
}

void handlePlaying(controls::Event e) {
    switch (e) {
        case controls::Event::EncoderCW:
            simSpeed += cfg::SPEED_STEP;
            if (simSpeed > cfg::SPEED_MAX) simSpeed = cfg::SPEED_MAX;
            break;
        case controls::Event::EncoderCCW:
            simSpeed -= cfg::SPEED_STEP;
            if (simSpeed < cfg::SPEED_MIN) simSpeed = cfg::SPEED_MIN;
            break;
        case controls::Event::EncoderPress:
            simSpeed = 1.0f;
            break;
        case controls::Event::EncoderLongPress:
            simKey = !simKey;
            break;
        case controls::Event::PlayPress:
            simPaused = !simPaused;
            break;
        case controls::Event::BackPress:
            screen = Screen::Browser;
            break;
        case controls::Event::CueLongPress:
            simCue = true;
            break;
        default: break;
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(200);

    // The real firmware gets Wire initialized by arduino-audiokit on pins
    // 33/32 (shared with the codec). The sim has no audiokit, so do it here.
    Wire.begin(33, 32);

    controls::begin();
    ui::begin();

    Serial.println("SIM: ready");
    redraw();
}

void loop() {
    auto e = controls::poll();
    if (e == controls::Event::None) return;
    if (screen == Screen::Browser) handleBrowser(e);
    else                           handlePlaying(e);
    redraw();
}
