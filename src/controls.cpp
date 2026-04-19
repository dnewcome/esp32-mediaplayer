#include "controls.h"
#include "config.h"

namespace controls {

namespace {

// --- encoder ---
volatile int8_t  encDelta  = 0;
volatile uint8_t encState  = 0;

// Full quadrature state table — 4 counts per detent on most modules.
const int8_t QTAB[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0,
};

void IRAM_ATTR encoderISR() {
    uint8_t s = (digitalRead(cfg::PIN_ENC_A) << 1) | digitalRead(cfg::PIN_ENC_B);
    encDelta += QTAB[(encState << 2) | s];
    encState = s;
}

// --- buttons ---
//
// State machine per button:
//   Released -> Pressed      : on debounced falling edge
//   Pressed  -> Released     : on rising edge before threshold -> emit short
//   Pressed  -> LongEmitted  : when held past threshold -> emit long
//   LongEmitted -> Released  : on rising edge, no event
//
// Buttons without a long-press binding emit short-press on release and never
// enter LongEmitted (their LongPress slot is Event::None — suppressed).

enum class State : uint8_t { Released, Pressed, LongEmitted };

constexpr uint32_t DEBOUNCE_MS   = 20;
constexpr uint32_t LONG_PRESS_MS = 400;

struct Btn {
    int         pin;
    Event       shortEvt;
    Event       longEvt;     // Event::None = no long-press binding
    State       state;
    bool        lastLevel;   // LOW=pressed
    uint32_t    lastChangeMs;
    uint32_t    pressStartMs;
};

Btn btns[] = {
    { cfg::PIN_ENC_SW,   Event::EncoderPress, Event::EncoderLongPress, State::Released, HIGH, 0, 0 },
    { cfg::PIN_BTN_PLAY, Event::PlayPress,    Event::None,             State::Released, HIGH, 0, 0 },
    { cfg::PIN_BTN_BACK, Event::BackPress,    Event::None,             State::Released, HIGH, 0, 0 },
    { cfg::PIN_BTN_CUE,  Event::CuePress,     Event::CueLongPress,     State::Released, HIGH, 0, 0 },
};
constexpr int NUM_BTNS = sizeof(btns) / sizeof(btns[0]);

Event stepButton(Btn& b) {
    const uint32_t now = millis();
    const bool cur = digitalRead(b.pin);

    if (cur != b.lastLevel && (now - b.lastChangeMs) > DEBOUNCE_MS) {
        b.lastChangeMs = now;
        b.lastLevel    = cur;

        if (cur == LOW) {                           // press
            b.state        = State::Pressed;
            b.pressStartMs = now;
        } else {                                    // release
            State prev = b.state;
            b.state = State::Released;
            if (prev == State::Pressed) return b.shortEvt;
            // prev == LongEmitted: long already fired, consume release.
        }
    }

    // Fire long-press once while held past threshold.
    if (b.state == State::Pressed && b.longEvt != Event::None
        && (now - b.pressStartMs) >= LONG_PRESS_MS) {
        b.state = State::LongEmitted;
        return b.longEvt;
    }
    return Event::None;
}

} // namespace

void begin() {
    pinMode(cfg::PIN_ENC_A,    INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_B,    INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_SW,   INPUT_PULLUP);
    pinMode(cfg::PIN_BTN_PLAY, INPUT_PULLUP);
    pinMode(cfg::PIN_BTN_BACK, INPUT_PULLUP);
    pinMode(cfg::PIN_BTN_CUE,  INPUT_PULLUP);

    encState = (digitalRead(cfg::PIN_ENC_A) << 1) | digitalRead(cfg::PIN_ENC_B);
    attachInterrupt(digitalPinToInterrupt(cfg::PIN_ENC_A), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(cfg::PIN_ENC_B), encoderISR, CHANGE);
}

Event poll() {
    // Drain encoder detents first.
    noInterrupts();
    int8_t d = encDelta;
    if (d >= 4)  { encDelta -= 4; interrupts(); return Event::EncoderCW; }
    if (d <= -4) { encDelta += 4; interrupts(); return Event::EncoderCCW; }
    interrupts();

    for (int i = 0; i < NUM_BTNS; ++i) {
        Event e = stepButton(btns[i]);
        if (e != Event::None) return e;
    }
    return Event::None;
}

} // namespace controls
