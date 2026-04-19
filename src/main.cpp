#include <Arduino.h>
#include <SD_MMC.h>

#include "config.h"
#include "controls.h"
#include "ui.h"
#include "player.h"

namespace {

char   fileBuf[cfg::MAX_FILES][cfg::MAX_FILENAME_LEN];
const char* files[cfg::MAX_FILES];
int    fileCount = 0;

enum class Screen { Browser, Playing };
Screen screen = Screen::Browser;
int    selected = 0;

bool hasAudioExt(const char* name) {
    int n = strlen(name);
    if (n < 5) return false;
    const char* e = name + n - 4;
    return strcasecmp(e, ".mp3") == 0 || strcasecmp(e, ".wav") == 0;
}

void scanSD() {
    fileCount = 0;
    if (!SD_MMC.begin("/sdcard", true /* 1-bit mode */)) {
        ui::showMessage("SD mount failed");
        return;
    }
    File root = SD_MMC.open("/");
    if (!root) return;
    File f;
    while (fileCount < cfg::MAX_FILES && (f = root.openNextFile())) {
        if (!f.isDirectory() && hasAudioExt(f.name())) {
            strncpy(fileBuf[fileCount], f.name(), cfg::MAX_FILENAME_LEN - 1);
            fileBuf[fileCount][cfg::MAX_FILENAME_LEN - 1] = '\0';
            files[fileCount] = fileBuf[fileCount];
            ++fileCount;
        }
        f.close();
    }
    root.close();
}

void redraw() {
    if (screen == Screen::Browser) {
        if (fileCount == 0) ui::showMessage("No audio files");
        else ui::showBrowser(files, fileCount, selected);
    } else {
        ui::showNowPlaying(files[selected], player::speed(), player::isPaused(),
                           player::mode() == player::Mode::Keylock,
                           player::hasCue());
    }
}

void playSelected() {
    if (fileCount == 0) return;
    char path[cfg::MAX_FILENAME_LEN + 2];
    snprintf(path, sizeof(path), "/%s", files[selected]);
    if (player::play(path)) {
        screen = Screen::Playing;
    } else {
        ui::showMessage("Play failed");
    }
}

void handleBrowser(controls::Event e) {
    switch (e) {
        case controls::Event::EncoderCW:
            if (selected + 1 < fileCount) ++selected;
            break;
        case controls::Event::EncoderCCW:
            if (selected > 0) --selected;
            break;
        case controls::Event::EncoderPress:
        case controls::Event::PlayPress:
            playSelected();
            break;
        default: break;
    }
}

void handlePlaying(controls::Event e) {
    switch (e) {
        case controls::Event::EncoderCW:
            player::setSpeed(player::speed() + cfg::SPEED_STEP);
            break;
        case controls::Event::EncoderCCW:
            player::setSpeed(player::speed() - cfg::SPEED_STEP);
            break;
        case controls::Event::EncoderPress:
            player::setSpeed(1.0f);  // snap to neutral
            break;
        case controls::Event::EncoderLongPress:
            // Toggle keylock/pitched mode. Takes effect on next play() —
            // restart the current track so the new pipeline takes over.
            player::setMode(player::mode() == player::Mode::Pitched
                            ? player::Mode::Keylock
                            : player::Mode::Pitched);
            ui::showMessage(player::mode() == player::Mode::Keylock
                            ? "Keylock ON" : "Keylock OFF");
            delay(600);
            playSelected();
            break;
        case controls::Event::PlayPress:
            player::togglePause();
            break;
        case controls::Event::BackPress:
            player::stop();
            screen = Screen::Browser;
            break;
        case controls::Event::CuePress:
            if (player::hasCue()) player::jumpToCue();
            else                  playSelected();   // no cue yet: restart
            break;
        case controls::Event::CueLongPress:
            player::setCuePoint();
            ui::showMessage("Cue set");
            delay(400);
            break;
        default: break;
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(200);

    controls::begin();
    ui::begin();
    player::begin();
    scanSD();
    redraw();
}

void loop() {
    player::loopTick();

    auto e = controls::poll();
    if (e == controls::Event::None) return;

    if (screen == Screen::Browser) handleBrowser(e);
    else                           handlePlaying(e);
    redraw();
}
