#pragma once
#include <Arduino.h>

namespace controls {

// Short-press events fire on release (before the long-press threshold).
// Long-press events fire once, at the moment the threshold is crossed.
enum class Event : uint8_t {
    None,
    EncoderCW,
    EncoderCCW,
    EncoderPress,       // short: tap
    EncoderLongPress,   // long: toggle playback mode
    PlayPress,
    BackPress,
    CuePress,           // short: jump to cue
    CueLongPress,       // long: set cue point at current position
};

void begin();
Event poll();

} // namespace controls
