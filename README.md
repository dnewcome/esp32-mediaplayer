# esp32-mediaplayer

An ESP32-A1S media player aimed at eventually driving a portable DJ turntable
with Serato timecode vinyl control. Plays MP3/WAV from an SD card with
variable-speed playback (pitched or pitch-preserving), OLED file browser, and
cue points.

The long-term target is a self-contained turntable deck: drop a control
record on the platter and the ESP32 decodes position and speed from the
timecode, playing the selected MP3 in perfect sync with the vinyl.

## Status

Scaffold-complete. Untested on hardware — awaiting a board.

| Area | State |
| --- | --- |
| PlatformIO project (ES8388 + AC101 build envs) | Done |
| MP3 + WAV playback from SD | Done |
| SSD1306 OLED browser + now-playing UI | Done |
| Rotary encoder + 3 buttons, long-press state machine | Done |
| Pitched variable speed (resampler) | Done |
| Keylock variable speed (WSOLA) | Implemented, unverified |
| Cue points (byte-level seek) | Done (glitches on MP3 mid-frame) |
| Timecode zero-crossing + speed/direction | Done |
| Timecode bit decode + position lookup | TODO (needs real vinyl samples) |
| Codec RX (line-in) path for timecode input | TODO |
| Host-side DSP test harness for WSOLA | TODO |

## Hardware

- **Board:** AI-Thinker ESP32-A1S Audio Kit. Two revisions ship with
  different codecs (ES8388 or AC101) with no PCB silkscreen difference —
  check the IC marking near the 3.5 mm jacks. Both are supported by
  separate PlatformIO environments.
- **SD card:** inserted in the A1S slot (uses SD-MMC 1-bit mode).
- **Display:** 128×64 SSD1306 OLED over I²C. Shares the codec's I²C bus
  (SDA = GPIO 33, SCL = GPIO 32). No conflict: SSD1306 lives at `0x3C`,
  ES8388 at `0x10`, AC101 at `0x1A`.
- **Input:** 1 rotary encoder with push switch + 3 push buttons.
  Internal pull-ups are used; wire buttons to GND.

### Pin assignments

Default pin mapping (see `include/config.h` to adjust):

| Function | GPIO |
| --- | --- |
| Encoder A | 19 |
| Encoder B | 23 |
| Encoder switch | 18 |
| Button: Play/Pause | 5 |
| Button: Back | 21 |
| Button: Cue | 22 |
| OLED SDA (shared with codec) | 33 |
| OLED SCL (shared with codec) | 32 |

These are all pins exposed on the A1S J1/J2 expansion headers. The codec,
I²S bus, and SD card use their own dedicated pins and should be left alone.

## Building

Install [PlatformIO](https://platformio.org/) (CLI or the VS Code
extension). Then pick the build environment that matches your A1S codec:

```bash
# ES8388 variant (more common on recent A1S boards)
pio run -e esp32-a1s-es8388 -t upload

# AC101 variant
pio run -e esp32-a1s-ac101 -t upload
```

Serial monitor:

```bash
pio device monitor -b 115200
```

Wrong environment? Audio will be silent or distorted even though the board
boots — re-flash with the other env.

## Controls

| Screen | Input | Action |
| --- | --- | --- |
| Browser | Encoder CW/CCW | Scroll file list |
| Browser | Encoder press / Play | Play selected file |
| Playing | Encoder CW/CCW | Nudge speed ±0.02× |
| Playing | Encoder short-press | Snap speed to 1.0× |
| Playing | Encoder long-press (400 ms) | Toggle keylock / pitched |
| Playing | Play | Pause / resume |
| Playing | Back | Stop and return to browser |
| Playing | Cue short-press | Jump to cue point (or restart if no cue) |
| Playing | Cue long-press | Set cue point at current position |

## How it works

### Audio pipeline

```
SD file ─► StreamCopy ─► EncodedAudioStream(MP3 or WAV)
                                    │
                                    ▼
                         ┌──── ResampleStream ────┐    (Pitched mode)
                         │                        │
                         └──── WsolaStream   ─────┘    (Keylock mode)
                                    │
                                    ▼
                           AudioBoardStream (I²S + codec)
```

The player wires both speed stages in parallel and routes the copier
to whichever matches the selected mode. Only the active stage drains input.

### WSOLA (Waveform-Similarity Overlap-Add)

Time-domain pitch-preserving stretching. For each output chunk:

1. Extract a 1024-sample analysis window from the input, starting near
   `analysisPos += HOP_SYN × speed`.
2. Search ±128 samples around that position for the window whose leading
   samples best match the natural continuation of the previous window
   (AMDF similarity on mono-summed input — cheaper than cross-correlation).
3. Hann-window the chosen frame and overlap-add onto the running output
   (75 % overlap).

Parameters (44.1 kHz): `FRAME_N = 1024`, `HOP_SYN = 256`, `SEARCH = 128`.
See `src/wsola.cpp`.

### Timecode decoder

Skeleton only — see `src/timecode.{h,cpp}`. Tracks zero crossings to
estimate carrier period (→ speed) and L/R phase relationship
(→ direction). Bit extraction and Serato-specific code table lookup are
left as a TODO; plan is to port the reverse-engineered tables from
[xwax](https://xwax.org/) once hardware is available to validate against.

## References / inspiration

- [pschatzmann/arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) —
  the audio framework this player is built on, especially the
  [`player-sd-audiokit`](https://github.com/pschatzmann/arduino-audio-tools/tree/main/examples/examples-player/player-sd-audiokit) example.
- [bbulkow/loudframe](https://github.com/bbulkow/loudframe) — reference for
  SD playback and variable-speed handling on the A1S.
- [xwax](https://xwax.org/) — reverse-engineered timecode decoding for
  Serato Control Vinyl (and other formats) that the eventual timecode
  implementation will draw on.
- Verhelst & Roelands, "An Overlap-Add Technique Based on Waveform
  Similarity (WSOLA) For High Quality Time-Scale Modification of Speech",
  ICASSP 1993.
