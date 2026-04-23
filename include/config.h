#pragma once

// Central pin + constant definitions. Adjust to match your exact A1S
// revision and external wiring.
//
// The A1S reserves many GPIOs for the codec, SD card, and I2S bus.
// Pins available on the J1/J2 expansion headers (on most A1S v2.2 boards):
//   GPIO 5, 18, 19, 21, 22, 23  (general-purpose)
//   GPIO 1, 3                   (UART — avoid; used for serial monitor)
//
// The codec's I2C bus (GPIO 33 SDA / 32 SCL) is shared with the OLED to
// avoid burning two more GPIOs. SSD1306 lives at 0x3C, ES8388 at 0x10,
// AC101 at 0x1A — no conflict.

namespace cfg {

// --- Audio ---
constexpr int SAMPLE_RATE     = 44100;
constexpr int CHANNELS        = 2;
constexpr int BITS_PER_SAMPLE = 16;

// --- Rotary encoder (with push switch) ---
constexpr int PIN_ENC_A  = 19;
constexpr int PIN_ENC_B  = 23;
constexpr int PIN_ENC_SW = 18;

// --- Buttons (active LOW, internal pull-ups) ---
constexpr int PIN_BTN_PLAY = 5;
constexpr int PIN_BTN_BACK = 21;
constexpr int PIN_BTN_CUE  = 22;

// --- Pitch / speed ---
// Phase 1: naive resampling — pitch changes with speed.
// Phase 2: WSOLA time-stretch decouples speed from pitch (keylock).
// SPEED_MIN was 0.5f but Proportional-mapped timecode needs much slower
// effective rates: a 3-minute track on a 12-minute Serato Vinyl plays
// at 3/12 = 0.25× effective when the tc walks at 1×. WSOLA handles
// that cleanly; audio quality degrades toward 0.1× but still usable.
constexpr float SPEED_MIN  = 0.1f;
constexpr float SPEED_MAX  = 2.0f;
constexpr float SPEED_STEP = 0.02f;

// --- File browser ---
constexpr int MAX_FILES       = 64;
constexpr int MAX_FILENAME_LEN = 48;

} // namespace cfg
