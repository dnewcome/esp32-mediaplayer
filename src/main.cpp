#include <Arduino.h>
#include <SD_MMC.h>

#include "config.h"
#include "controls.h"
#include "ui.h"
#include "player.h"
#include "timecode_in.h"

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

// Recursive scan: walk the SD tree, collect files with audio extensions.
// Relative paths (without leading '/') are stored so they fit in
// MAX_FILENAME_LEN; player::play() re-adds the slash when opening.
// Every entry visited is logged to Serial so we can tell at a glance
// whether the card is empty, full of unexpected names, or holding
// files in a subfolder we'd otherwise miss.
void scanDir(File dir, const char* prefix) {
    File f;
    while (fileCount < cfg::MAX_FILES && (f = dir.openNextFile())) {
        const char* name = f.name();
        if (f.isDirectory()) {
            Serial.print(F("  [dir]  "));
            Serial.println(name);
            char sub[cfg::MAX_FILENAME_LEN];
            snprintf(sub, sizeof(sub), "%s%s/",
                     prefix[0] ? prefix : "", name);
            scanDir(f, sub);
        } else {
            const bool audio = hasAudioExt(name);
            Serial.print(audio ? F("  [ok ]  ") : F("  [skip] "));
            Serial.print(prefix);
            Serial.println(name);
            if (audio) {
                char full[cfg::MAX_FILENAME_LEN];
                snprintf(full, sizeof(full), "%s%s", prefix, name);
                strncpy(fileBuf[fileCount], full, cfg::MAX_FILENAME_LEN - 1);
                fileBuf[fileCount][cfg::MAX_FILENAME_LEN - 1] = '\0';
                files[fileCount] = fileBuf[fileCount];
                ++fileCount;
            }
        }
        f.close();
    }
}

void scanSD() {
    fileCount = 0;
    Serial.println(F("\n[sd] mounting..."));
    if (!SD_MMC.begin("/sdcard", true /* 1-bit mode */)) {
        Serial.println(F("[sd] mount FAILED — is a card inserted?"));
        ui::showMessage("SD mount failed");
        return;
    }
    Serial.print(F("[sd] mounted, type="));
    switch (SD_MMC.cardType()) {
        case CARD_NONE: Serial.println(F("NONE")); break;
        case CARD_MMC:  Serial.println(F("MMC"));  break;
        case CARD_SD:   Serial.println(F("SD"));   break;
        case CARD_SDHC: Serial.println(F("SDHC")); break;
        default:        Serial.println(F("UNK"));  break;
    }
    Serial.print(F("[sd] size="));
    Serial.print((uint32_t)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
    Serial.println(F(" MiB"));

    File root = SD_MMC.open("/");
    if (!root) { Serial.println(F("[sd] could not open /")); return; }
    scanDir(root, "");
    root.close();

    Serial.print(F("[sd] found "));
    Serial.print(fileCount);
    Serial.println(F(" audio file(s)"));
}

// Mirror the OLED UI to Serial so the firmware is usable without a
// display. Printed on every state change; a short `?` prints the key
// cheatsheet on demand.
void printSerialUI() {
    Serial.println();
    if (screen == Screen::Browser) {
        Serial.print(F("== Browser ==  "));
        Serial.print(fileCount == 0 ? 0 : selected + 1);
        Serial.print('/');
        Serial.println(fileCount);
        if (fileCount == 0) { Serial.println(F("  (no audio files)")); return; }
        int start = selected - 2; if (start < 0) start = 0;
        int end   = start + 5;    if (end > fileCount) end = fileCount;
        for (int i = start; i < end; ++i) {
            Serial.print(i == selected ? F(" > ") : F("   "));
            Serial.println(files[i]);
        }
    } else {
        Serial.print(F("== Playing ==  "));
        Serial.println(files[selected]);
        Serial.print(F("  speed="));  Serial.print(player::speed(), 2);
        Serial.print(F("x  mode=")); Serial.print(
            player::mode() == player::Mode::Keylock ? F("keylock") : F("pitched"));
        Serial.print(F("  "));        Serial.print(
            player::isPaused() ? F("PAUSED") : F("PLAYING"));
        Serial.print(F("  cue="));    Serial.print(player::hasCue() ? F("yes") : F("no"));
        Serial.print(F("  tc="));
        if (!timecode_in::enabled())     Serial.println(F("off"));
        else if (timecode_in::locked())  { Serial.print(F("locked@"));
                                           Serial.print(timecode_in::speed(), 2);
                                           Serial.println('x'); }
        else                              Serial.println(F("searching"));
    }
}

void printHelp() {
    Serial.println(F(
        "\nkeys:\n"
        "  browser: w/s scroll, enter play\n"
        "  playing: +/- speed, = snap-1.0, K keylock, p pause,\n"
        "           b back, c cue, C set-cue\n"
        "  global:  t timecode arm, r rescan SD, ? help\n"));
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
    printSerialUI();
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
    timecode_in::begin();
    scanSD();
    printHelp();
    redraw();
}

namespace {

// Map a single char from Serial to a controls::Event in the current
// screen's vocabulary. Returns Event::None if the char isn't bound.
controls::Event keyToEvent(int c) {
    if (screen == Screen::Browser) {
        switch (c) {
            case 'w': case 'k':    return controls::Event::EncoderCCW;
            case 's': case 'j':    return controls::Event::EncoderCW;
            case '\r': case '\n':  return controls::Event::EncoderPress;
            default: break;
        }
    } else {
        switch (c) {
            case '+':              return controls::Event::EncoderCW;
            case '-':              return controls::Event::EncoderCCW;
            case '=':              return controls::Event::EncoderPress;
            case 'K':              return controls::Event::EncoderLongPress;
            case 'p': case ' ':    return controls::Event::PlayPress;
            case 'b':              return controls::Event::BackPress;
            case 'c':              return controls::Event::CuePress;
            case 'C':              return controls::Event::CueLongPress;
            default: break;
        }
    }
    return controls::Event::None;
}

// Drain serial. Single-char keybindings mirror the OLED controls, plus
// a couple of dev toggles ('t' timecode, '?' help) that don't have a
// physical-button equivalent yet.
void pollSerial() {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '?') { printHelp(); continue; }
        if (c == 'r') { scanSD(); selected = 0; redraw(); continue; }
        if (c == 't') {
            bool on = !timecode_in::enabled();
            timecode_in::setEnabled(on);
            Serial.print(F("timecode_in: "));
            Serial.println(on ? F("ON") : F("OFF"));
            continue;
        }
        auto e = keyToEvent(c);
        if (e == controls::Event::None) continue;
        if (screen == Screen::Browser) handleBrowser(e);
        else                           handlePlaying(e);
        redraw();
    }
}

// When timecode lock is held, drive player speed from the vinyl. Only
// applies during playback; if the deck isn't playing there's nothing
// to steer. Speed is clamped by player::setSpeed().
void driveFromTimecode() {
    if (!timecode_in::enabled() || !timecode_in::locked()) return;
    if (!player::isPlaying()) return;
    player::setSpeed(timecode_in::speed());
}

} // namespace

void loop() {
    player::loopTick();
    timecode_in::tick();
    driveFromTimecode();
    pollSerial();

    // Return to the browser when the current track hits EOF. player::stop()
    // fires from loopTick() but only touches audio state, not the screen.
    if (screen == Screen::Playing && !player::isPlaying()) {
        screen = Screen::Browser;
        redraw();
    }

    // Playback liveness trace: once a second while on the Playing screen,
    // print the file byte position so we can confirm the copier is draining
    // bytes even with no speaker attached.
    static uint32_t lastTraceMs = 0;
    if (screen == Screen::Playing && player::isPlaying() && !player::isPaused()) {
        uint32_t now = millis();
        if (now - lastTraceMs >= 1000) {
            lastTraceMs = now;
            Serial.print(F("[play] pos="));
            Serial.print(player::filePosition());
            Serial.print(F("  speed="));
            Serial.print(player::speed(), 2);
            Serial.println('x');
        }
    }

    auto e = controls::poll();
    if (e == controls::Event::None) return;

    if (screen == Screen::Browser) handleBrowser(e);
    else                           handlePlaying(e);
    redraw();
}
