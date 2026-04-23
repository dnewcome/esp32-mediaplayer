#include <Arduino.h>
#include <SD_MMC.h>

#include "codec.h"
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
        "  playing: +/- speed (fine), [/] speed (coarse), = snap-1.0,\n"
        "           < > hold-to-nudge (-/+2%), p pause, b back, c cue, C set-cue\n"
        "  global:  K keylock toggle, t timecode arm, f cycle tc flags,\n"
        "           L local-loop (feed decoder from /timecode.wav, no ADC),\n"
        "           F cycle tc format (vinyl/CD),\n"
        "           P toggle transport mode (absolute/proportional),\n"
        "           S seek to track midpoint (manual seekToMs test),\n"
        "           D dump ES8388 regs, g/G input gain -/+,\n"
        "           v/V output vol -/+, r rescan SD, ? help\n"));
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

    // Log PSRAM availability — timecode::lookupPosition's LUT needs ~4 MB.
    Serial.printf("[boot] PSRAM total=%u free=%u (need ~4 MB for tc position LUT)\n",
                  (unsigned)ESP.getPsramSize(),
                  (unsigned)ESP.getFreePsram());

    controls::begin();
    ui::begin();
    player::begin();
    timecode_in::begin();
    scanSD();
    printHelp();
    redraw();
}

namespace {

// --- Nudge: hold '>' / '<' to bump playback ±NUDGE_AMOUNT temporarily (DJ
// beat-matching). We can't see key release over a line-discipline serial
// terminal, so we rely on the terminal's key-repeat: each repeat refreshes
// a timeout; when it stops firing we snap back to the pre-nudge speed.
// ~30 Hz repeat rate means a timeout of ~180 ms catches the gap reliably.
constexpr uint32_t NUDGE_TIMEOUT_MS = 180;
constexpr float    NUDGE_AMOUNT     = 0.02f;
bool     nudging_    = false;
int      nudgeDir_   = 0;
float    nudgeBase_  = 1.0f;
uint32_t nudgeLast_  = 0;

void nudgePress(int dir) {
    if (!player::isPlaying()) return;
    if (!nudging_ || dir != nudgeDir_) {
        nudgeBase_ = player::speed();
        nudgeDir_  = dir;
        nudging_   = true;
    }
    nudgeLast_ = millis();
    player::setSpeed(nudgeBase_ + dir * NUDGE_AMOUNT);
}

void nudgeTick() {
    if (!nudging_) return;
    if (millis() - nudgeLast_ < NUDGE_TIMEOUT_MS) return;
    player::setSpeed(nudgeBase_);
    nudging_ = false;
    nudgeDir_ = 0;
    redraw();
}

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
        if (c == 'D') { codec::dumpRegs(); continue; }
        // --- DAC→ADC coupling probes. Manipulate ES8388 regs at runtime
        //     so we can A/B which output stage is the dominant coupling
        //     path without reflashing.
        //   1: only HP (LOUT1/ROUT1) enabled — Dan's default monitoring
        //      path. If rx drops, LINE OUT stage was the coupler.
        //   2: only LINE OUT (LOUT2/ROUT2) — HP goes silent, rx should
        //      tell whether HP stage was coupling.
        //   3: all outputs (default, 0x3C).
        //   M: mute DAC (LDACVOL/RDACVOL → -96dB). Impossibility check:
        //      rx MUST drop — if not, there's a non-DAC path at play.
        //   U: un-mute DAC.
        if (c == '1') { codec::writeReg(0x04, 0x0C); Serial.println(F("reg04=0x0C (HP only)")); continue; }
        if (c == '2') { codec::writeReg(0x04, 0x30); Serial.println(F("reg04=0x30 (LINE OUT only)")); continue; }
        if (c == '3') { codec::writeReg(0x04, 0x3C); Serial.println(F("reg04=0x3C (all outputs)")); continue; }
        if (c == 'M') { codec::writeReg(0x1A, 0xC0); codec::writeReg(0x1B, 0xC0); Serial.println(F("DAC vol = -96dB (muted)")); continue; }
        if (c == 'U') { codec::writeReg(0x1A, 0x00); codec::writeReg(0x1B, 0x00); Serial.println(F("DAC vol = 0dB")); continue; }
        if (c == 'f') {
            uint32_t f = timecode_in::cycleFlags();
            Serial.print(F("tc flags=0x"));
            Serial.print(f, HEX);
            Serial.print(F("  ("));
            Serial.print((f & 0x1) ? F("PHASE ")    : F("       "));
            Serial.print((f & 0x2) ? F("PRIMARY ")  : F("        "));
            Serial.print((f & 0x4) ? F("POLARITY")  : F("        "));
            Serial.println(')');
            continue;
        }
        if (c == 'g') { codec::adjustInputGain(-10); continue; }
        if (c == 'G') { codec::adjustInputGain(+10); continue; }
        if (c == 'v') { codec::adjustOutputVolume(-10); continue; }
        if (c == 'V') { codec::adjustOutputVolume(+10); continue; }
        if (c == 't') {
            bool on = !timecode_in::enabled();
            timecode_in::setEnabled(on);
            Serial.print(F("timecode_in: "));
            Serial.println(on ? F("ON") : F("OFF"));
            continue;
        }
        if (c == 'L') {
            // Local-loop diagnostic: feed decoder from /timecode.wav on SD
            // instead of the ADC. Lets us test position/seek/mapping logic
            // without hardware. Task prints confirmation on successful open.
            bool on = !timecode_in::localLoop();
            timecode_in::setLocalLoop(on);
            Serial.print(F("local-loop request: "));
            Serial.println(on ? F("ON") : F("OFF"));
            continue;
        }
        if (c == 'F') {
            // Cycle decoder format Vinyl ↔ CD. Resets decoder state.
            // Required to match /timecode.wav (CD-format) in local-loop.
            timecode_in::cycleFormat();
            Serial.print(F("tc format: "));
            Serial.println(timecode_in::isCdFormat() ? F("CD") : F("VINYL"));
            continue;
        }
        if (c == 'S') {
            // Seek to track midpoint — manual test for seekToMs. Phase 6
            // will drive seek from the timecode position automatically.
            uint32_t dur = player::trackDurationMs();
            uint32_t target = dur / 2;
            bool ok = player::seekToMs(target);
            Serial.print(F("seekToMs(")); Serial.print(target);
            Serial.print(F(") → "));
            Serial.println(ok ? F("OK") : F("refused (not playing or past end)"));
            continue;
        }
        if (c == 'P') {
            // Toggle Absolute ↔ Proportional position mapping.
            bool abs = (player::transportMode() == player::TransportMode::Absolute);
            player::setTransportMode(abs ? player::TransportMode::Proportional
                                          : player::TransportMode::Absolute);
            Serial.print(F("transport mode: "));
            Serial.println(abs ? F("PROPORTIONAL") : F("ABSOLUTE"));
            continue;
        }
        if (c == 'K') {
            // Global keylock toggle. setMode() only stores the choice; the
            // pipeline rebinds on the next play(). If a track is already
            // playing, restart it so the new stage takes over immediately.
            player::setMode(player::mode() == player::Mode::Pitched
                            ? player::Mode::Keylock
                            : player::Mode::Pitched);
            Serial.print(F("keylock: "));
            Serial.println(player::mode() == player::Mode::Keylock ? F("ON") : F("OFF"));
            if (screen == Screen::Playing) playSelected();
            redraw();
            continue;
        }
        if (screen == Screen::Playing) {
            if (c == ']') { player::setSpeed(player::speed() + 0.1f); redraw(); continue; }
            if (c == '[') { player::setSpeed(player::speed() - 0.1f); redraw(); continue; }
            if (c == '>') { nudgePress(+1); continue; }
            if (c == '<') { nudgePress(-1); continue; }
        }
        auto e = keyToEvent(c);
        if (e == controls::Event::None) continue;
        if (screen == Screen::Browser) handleBrowser(e);
        else                           handlePlaying(e);
        redraw();
    }
}

// Map a timecode cycle-index position to a track millisecond target,
// per the user's selected TransportMode. Pure function — no I/O,
// easy to test mentally.
//
//   Absolute     : target = tc_ms. Caller handles the case where
//                  target > track_dur_ms (stop at end per plan).
//   Proportional : target = tc_ms × track_dur / vinyl_len. Assumes
//                  tc_ms is bounded by vinyl_len_ms (decoder can't
//                  report positions past that; they'd re-lock at the
//                  wrap-around state).
uint32_t mapTcToTrackMs(uint32_t tc_ms,
                        uint32_t track_dur_ms,
                        uint32_t vinyl_len_ms,
                        player::TransportMode mode) {
    if (mode == player::TransportMode::Absolute) {
        return tc_ms;
    }
    if (vinyl_len_ms == 0) return 0;
    return (uint32_t)((uint64_t)tc_ms * track_dur_ms / vinyl_len_ms);
}

// When timecode lock is held, drive player speed from the vinyl. Only
// applies during playback; if the deck isn't playing there's nothing
// to steer. Speed is clamped by player::setSpeed().
//
// Hysteresis: one-window lock flickers (a momentary bit error during
// platter spin-up) can latch a bad speed and never get cleared, freezing
// playback at something like 0.90×. So we only steer after lock has
// been held continuously for LOCK_HOLD_MS, and revert to 1.0× after
// LOCK_DROP_MS of continuous unlock (treat as "needle up / signal gone").
// Gate on *speed validity*, not bit-lock. Zero-crossing timing (speed
// estimate) is robust once the platter is moving; bit-level lock is
// fragile and often fails to latch at all even when speed is being
// estimated correctly. Steer while speed is in a sane DJ-platter range;
// revert to 1.0× after a run of insane values (needle up / motor off).
void driveFromTimecode() {
    if (!player::isPlaying()) return;

    constexpr uint32_t APPLY_INTERVAL_MS = 50;     // min gap between reclocks
    constexpr uint32_t RELEASE_MS        = 500;    // insane-speed hold-over
    constexpr float    SPEED_MIN         = 0.1f;
    constexpr float    SPEED_MAX         = 2.0f;
    // Don't reclock for wiggle smaller than this. Decoder speed estimate
    // jitters ±0.5% around nominal even at a steady platter; forwarding
    // every jitter to setSpeed reclocks I²S thousands of times per
    // minute, which is the audible "glitchy pitch" symptom.
    constexpr float    DELTA_EPSILON     = 0.01f;  // 1 % change minimum
    // EMA smoothing factor on decoder magnitude. 0.15 cuts raw jitter
    // by ~3× at steady state while still catching a platter nudge
    // within ~10 updates (~100 ms at typical decoder report rate).
    constexpr float    SPEED_EMA_ALPHA   = 0.15f;

    static uint32_t lastApplyMs  = 0;
    static uint32_t lastSaneMs   = 0;
    static float    lastApplied  = 1.0f;
    static float    smoothed     = 1.0f;
    static bool     steering     = false;

    if (!timecode_in::enabled()) {
        if (steering) { player::setSpeed(1.0f); steering = false; lastApplied = 1.0f; smoothed = 1.0f; }
        return;
    }

    const float    s    = timecode_in::speed();
    // Work in magnitude — player::setSpeed clamps negative values up to
    // SPEED_MIN, which was the cause of the "stuck at 0.5×" bug when
    // SWITCH_PRIMARY flipped the sign. Reverse-scratch (negative speed
    // playback) is a separate Phase 2 feature; for now, platter spinning
    // either direction should pitch the track forward.
    const float    m    = s < 0 ? -s : s;
    const uint32_t now  = millis();
    const bool     sane = (m >= SPEED_MIN && m <= SPEED_MAX);

    if (sane) {
        lastSaneMs = now;
        // EMA on the magnitude. Raw decoder speed has ~1% jitter per
        // report window even on a clean lock; updating the player on
        // every raw sample churns WSOLA and reads as audible warble.
        smoothed = smoothed + SPEED_EMA_ALPHA * (m - smoothed);
        const float delta = smoothed > lastApplied ? smoothed - lastApplied : lastApplied - smoothed;
        if (delta >= DELTA_EPSILON && now - lastApplyMs >= APPLY_INTERVAL_MS) {
            player::setSpeed(smoothed);
            lastApplyMs = now;
            lastApplied = smoothed;
            steering    = true;
        }
    } else if (steering && (now - lastSaneMs >= RELEASE_MS)) {
        player::setSpeed(1.0f);
        lastApplied = 1.0f;
        smoothed    = 1.0f;
        steering    = false;
    }
}

} // namespace

void loop() {
    player::loopTick();
    timecode_in::tick();
    driveFromTimecode();
    pollSerial();
    nudgeTick();

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
            Serial.print(F("  ms="));
            Serial.print(player::positionMs());
            Serial.print('/');
            Serial.print(player::trackDurationMs());
            Serial.print(F("  speed="));
            Serial.print(player::speed(), 2);
            Serial.println('x');
        }
    }

    // Timecode bring-up trace: once a second while timecode_in is armed,
    // independent of playback. Prints decoder state plus raw rx peak and
    // frames-per-window so we can tell "no signal" from "locked" from
    // "seeing signal but not locked yet" without opening a track.
    static uint32_t lastTcTraceMs = 0;
    if (timecode_in::enabled()) {
        uint32_t now = millis();
        if (now - lastTcTraceMs >= 1000) {
            lastTcTraceMs = now;
            auto st = timecode_in::takeStats();
            int16_t txPeak = codec::takeTxPeak();
            Serial.print(F("[tc] speed="));
            Serial.print(timecode_in::speed(), 3);
            Serial.print(F("  locked="));
            Serial.print(timecode_in::locked() ? 1 : 0);
            Serial.print(F("  rx="));
            Serial.print(st.peak);
            Serial.print(F("  tx="));
            Serial.print(txPeak);
            Serial.print(F("  pos="));
            Serial.print(timecode_in::position());  // cycle index, -1 until LUT lock
            Serial.print(F("  frames="));
            Serial.println(st.frames);
        }
    }

    auto e = controls::poll();
    if (e == controls::Event::None) return;

    if (screen == Screen::Browser) handleBrowser(e);
    else                           handlePlaying(e);
    redraw();
}
