#pragma once
#include <Arduino.h>

namespace player {

// Speed adjustment strategy.
//   Pitched : fast/slow playback *and* fast/slow pitch (varispeed — like a
//             record turntable). Zero CPU cost beyond resampling.
//   Keylock : playback speed changes, pitch stays constant. Uses WSOLA.
//             Noticeably more CPU; quality degrades at extreme ratios.
enum class Mode { Pitched, Keylock };

// How timecode position maps to track position (used by Phase 6's
// driveFromTimecode seek logic).
//   Absolute     : 1 ms of timecode = 1 ms of track. Classic DVS feel.
//                  Tracks shorter than the timecode (e.g. 3-min track
//                  on 12-min Serato vinyl) stop at end when tc pos
//                  exceeds track dur.
//   Proportional : Full timecode range maps to full track range. A
//                  3-min track covers the whole 12-min vinyl at a
//                  4× compression factor.
enum class TransportMode { Absolute, Proportional };

void begin();
void loopTick();

bool play(const char* absolutePath);
void togglePause();
void stop();

void  setSpeed(float s);   // 0.5 .. 2.0
float speed();

void  setMode(Mode m);     // takes effect on next play()
Mode  mode();

void          setTransportMode(TransportMode m);
TransportMode transportMode();

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

// Track duration / playback position in milliseconds.
//   trackDurationMs : latched at play() from WAV header or first MP3
//                     frame bitrate. 0 if not determined (unsupported
//                     format, MP3 bitrate parse failed, stopped).
//   positionMs      : current playback position, derived from the
//                     current byte offset and the cached sample rate
//                     (WAV) or cached bitrate (MP3). 0 when stopped.
// Both are used by driveFromTimecode's position-based seek logic
// once Phase 4-6 land; exposed now so Phase 3 is a separate commit.
uint32_t trackDurationMs();
uint32_t positionMs();

// Time-based seek. Converts `ms` to a byte offset using the cached
// WAV/MP3 header info and `audioFile.seek()`s there. Resets the WSOLA
// overlap-add state. Returns false if not playing, if the format's
// header hasn't been parsed, or if `ms` is past the track end. WAV:
// sample-accurate (modulo sub-ms rounding). MP3: the decoder re-syncs
// on the next 0xFFE frame header, producing a brief audible click.
bool seekToMs(uint32_t ms);

} // namespace player
