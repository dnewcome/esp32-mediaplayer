#pragma once
#include <Arduino.h>

namespace player {

// Speed adjustment strategy.
//   Pitched : fast/slow playback *and* fast/slow pitch (varispeed — like a
//             record turntable). Zero CPU cost beyond resampling.
//   Keylock : playback speed changes, pitch stays constant. Uses WSOLA.
//             Noticeably more CPU; quality degrades at extreme ratios.
enum class Mode { Pitched, Keylock };

void begin();
void loopTick();

bool play(const char* absolutePath);
void togglePause();
void stop();

void  setSpeed(float s);   // 0.5 .. 2.0
float speed();

void  setMode(Mode m);     // takes effect on next play()
Mode  mode();

// Cue points: stored per-playback (cleared on stop / track change).
// setCuePoint captures the current file byte position; jumpToCue seeks back.
// MP3 seek lands on an arbitrary byte — the decoder will resync on the next
// valid frame header, producing a brief glitch. WAV seeks are sample-accurate
// modulo the header.
void setCuePoint();
void jumpToCue();
bool hasCue();

bool isPlaying();
bool isPaused();

// Byte offset into the source file. Useful as a liveness signal: if
// this climbs, the copier is actually draining bytes. 0 when stopped.
uint32_t filePosition();

} // namespace player
