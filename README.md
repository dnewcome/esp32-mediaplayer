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
| Keylock variable speed (WSOLA) | Done (host-validated via SDL player) |
| Cue points (byte-level seek) | Done (glitches on MP3 mid-frame) |
| Timecode: speed + direction | Done (validated on Serato CD) |
| Timecode: bit decode + absolute position | Done host-side; ESP32 LUT pending |
| Codec RX (line-in) path for timecode input | TODO |
| Host simulation (SDL GUI + DSP harnesses) | Done |

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

### Serial interface

For bringing up an A1S without an OLED wired: the firmware mirrors the
UI to the 115200-baud serial console and accepts single-char keys.
`pio device monitor -b 115200`, then:

| Screen | Key | Action |
| --- | --- | --- |
| Browser | `w` / `s` | Scroll up / down |
| Browser | Enter | Play selected |
| Playing | `+` / `-` | Nudge speed ±0.02× |
| Playing | `[` / `]` | Nudge speed ±0.10× (coarse) |
| Playing | `=` | Snap speed to 1.0× |
| Playing | `K` | Toggle keylock / pitched |
| Playing | `p` or space | Pause / resume |
| Playing | `b` | Back to browser |
| Playing | `c` | Jump to cue |
| Playing | `C` | Set cue at current position |
| Any | `t` | Arm / disarm timecode input |
| Any | `?` | Print key help |

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

### CPU budget (unmeasured)

Rough estimates on a 240 MHz ESP32, everything running on core 1
(Arduino's main loop); core 0 is idle since we don't use WiFi/BT.

| Path                  | Est. one-core utilization |
| --------------------- | ------------------------- |
| Pitched mode          | ~20–30% (MP3 decode + DMA I²S) |
| Keylock mode          | ~35–50% (+ WSOLA: AMDF search dominates) |
| Timecode armed        | small — ZC + LFSR, a few % |

WSOLA's cost is dominated by the AMDF search: `FRAME_N × 2·SEARCH`
abs-diffs per `HOP_SYN` output samples on a mono-summed input, roughly
1K ops per output sample. Not measured on hardware yet — wrap a probe
around `player::loopTick()` with `esp_timer_get_time()` when we want a
real number.

### Timecode decoder

Implements Serato Control Vinyl and Serato Control CD decoding — speed,
direction, and absolute position on the record/CD. Source:
`src/timecode.{h,cpp}`.

```
int16 stereo in ─► per-channel ZC with hysteresis + DC-tracking zero
                   │                              │
                   ▼                              ▼
           direction (primary vs          period between primary ZCs
            secondary polarity at ZC)     → carrier speed (EMA-smoothed)
                   │                              │
                   ▼                              │
         bit sample at secondary ZC               │
         (|primary| vs running ref_level → 1/0)   │
                   │                              │
                   ▼                              │
         20-bit shift register + LFSR predictor   │
         (VALID_BITS = 24 consecutive matches     │
          before position is trusted)             │
                   │                              │
                   ▼                              │
         LUT lookup ► absolute cycle position ◄───┘
```

**Supported formats** (constants are published reverse-engineered facts
about Serato's commercial products):

| Format | Carrier | Bits | Seed | Taps | Length |
| --- | --- | --- | --- | --- | --- |
| Serato Control Vinyl 2nd ed. | 1 kHz | 20 | `0x59017` | `0x361e4` | 712 000 cycles |
| Serato Control CD | 1 kHz | 20 | `0xd8b40` | `0x34d54` | 950 000 cycles |

Resolution is one decoded cycle per millisecond of reference playback
(1 kHz carrier). With `VALID_BITS = 24`, lock takes ~25 ms of correct
bits plus ref-level EMA settling — measured at ~160 ms cold-start on a
clean capture.

**Host validation.** A reference 980 s capture of the Serato CD in the
repo (`timecode.wav`, not committed — supply your own) is decoded by
`native/timecode_analyze`; seek-offset positions decode linearly across
the usable range of the disc with perfect inter-sample consistency. A
constant LFSR-to-disc offset (≈2604 cycles for this pressing) is
absorbed when the player anchors position at playback start.

**ESP32 caveat.** The position LUT is host-only today —
`std::unordered_map` with ~30 MB footprint for `serato_cd`. The ESP32
path returns `position() = -1` while still producing correct speed and
direction. A PSRAM-resident compact LUT is the remaining work to ship
position decode on hardware.

**Licensing.** This is a clean-room implementation of the algorithm
described by [xwax](https://xwax.org/) — no xwax source was copied.
xwax is GPL-3; its code cannot be merged into this project without
relicensing. The format constants above are facts about Serato's
products, not copyrightable expression, and appear in multiple
independent open-source DVS implementations.

## Host simulation and testing

The firmware is structured so the DSP modules (`src/wsola.{h,cpp}` and
`src/timecode.{h,cpp}`) build cleanly against a host toolchain via
`-DWSOLA_NATIVE -DTIMECODE_NATIVE`, letting us validate them without
hardware. Host artifacts live under `native/` (SDL + gcc) and `sim/`
(Wokwi headless simulation).

### native/ — gcc + SDL harnesses

```
make -C native            # builds everything
```

Produces four binaries:

| Binary | Purpose |
| --- | --- |
| `wsola_play <file.wav> [speed]` | Feed a WAV through `Wsola`, emit raw PCM to stdout. Pipe to `aplay -f S16_LE -c 2 -r 44100`. |
| `mediaplayer ./media` | SDL 640×360 window reproducing the ESP32 UI (browser + playing screen) against a real WAV directory. Keyboard stands in for encoder/buttons. |
| `timecode_test` | Synthetic stereo-carrier smoke test — exercises ZC, direction, and period estimation across 500/1000/1500/2000 Hz. |
| `timecode_analyze <file.wav> [hop_ms]` | Stream mode: print speed/locked/position time series. |
| `timecode_analyze <file.wav> seek <off_s> [dur_s]` | Seek mode: decode a window, compare decoded position to ground-truth cycle offset. |
| `vinyl_demo <tc.wav> <music.wav> <tc_speed>` | End-to-end loop: resamples TC at `tc_speed` (simulating platter spin), decodes it, drives WSOLA on the music — music plays keylocked at the decoded speed. Stereo PCM to stdout. |

The same `src/wsola.cpp` and `src/timecode.cpp` that ship to the ESP32
are compiled here — only Arduino and AudioTools includes are guarded
out. One source of truth for DSP correctness.

### sim/ — Wokwi headless simulation

`sim/diagram.json` + `sim/wokwi.toml` + `sim/scenario.test.yaml` drive
the Wokwi CLI against the `esp32-sim` PlatformIO environment
(`src/main_sim.cpp` replaces `main.cpp`, strips audio deps, keeps UI +
controls). Serial assertions verify the state machine.

```
make -C native run_wokwi    # builds esp32-sim firmware and runs scenario
```

Needs `WOKWI_CLI_TOKEN` in `./.env` (gitignored); the target bails with
a clear error if it's missing.

This validates the non-audio half of the firmware (OLED rendering,
encoder/button handling, state transitions). Wokwi does **not**
simulate the ESP32 I²S peripheral — `mediaplayer` in `native/` is where
audio behaviour is exercised.

Common run targets (all under `make -C native`):

| Target | What it does |
| --- | --- |
| `run_sim` | Launch the SDL mediaplayer against `native/media/` |
| `run_wokwi` | Build esp32-sim firmware and run the Wokwi scenario |
| `run_play WAV=x.wav SPEED=1.25` | Pipe `wsola_play` output to `aplay` |
| `run_tc` | Run the synthetic-carrier timecode smoke test |
| `run_tc_wav TC_WAV=path.wav` | Stream a real capture through `timecode_analyze` |
| `run_vinyl_demo DEMO_SPEED=1.25 MUSIC=path.wav` | TC drives music playback (keylocked) via WSOLA — piped to aplay |

## References / inspiration

- [pschatzmann/arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) —
  the audio framework this player is built on, especially the
  [`player-sd-audiokit`](https://github.com/pschatzmann/arduino-audio-tools/tree/main/examples/examples-player/player-sd-audiokit) example.
- [bbulkow/loudframe](https://github.com/bbulkow/loudframe) — reference for
  SD playback and variable-speed handling on the A1S.
- [xwax](https://xwax.org/) — algorithmic reference for the timecode
  decoder. Our implementation is a clean-room rewrite (GPL-3 xwax code
  is not included); the Serato format constants are reverse-engineered
  facts originally published there.
- Verhelst & Roelands, "An Overlap-Add Technique Based on Waveform
  Similarity (WSOLA) For High Quality Time-Scale Modification of Speech",
  ICASSP 1993.
