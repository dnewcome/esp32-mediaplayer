# Timecode control

DJ-style transport from a Serato Control Vinyl (or CD) into the ESP32-A1S.
Turntable position drives track position; turntable speed drives track
tempo; needle-up pauses; needle-down resumes and re-seeks.

This doc is the reference for what's in the codebase today — the user-
visible behavior, the architectural decisions, the failure modes we
chased, and how to reproduce any of it. Read in order for the full
picture; skip around with the TOC for specific topics.

## TOC

1. [Signal pipeline](#signal-pipeline)
2. [Why Keylock and not Pitched](#why-keylock-and-not-pitched)
3. [DAC→ADC crosstalk](#dacadc-crosstalk)
4. [Position LUT on ESP32](#position-lut-on-esp32)
5. [Track duration, seek, and mapping](#track-duration-seek-and-mapping)
6. [`driveFromTimecode` control loop](#drivefromtimecode-control-loop)
7. [Local-loop diagnostic mode](#local-loop-diagnostic-mode)
8. [Runtime key reference](#runtime-key-reference)
9. [Test harnesses (`tools/`)](#test-harnesses-tools)
10. [Known limitations](#known-limitations)

---

## Signal pipeline

```
turntable cartridge
        │
        ▼  (line level, ~0.5 V rms)
  3.5 mm aux-in jack
        │
        ▼
  ES8388 LINPUT2 / RINPUT2        ← input must be LINE2, not the default LINE1
        │                           (codec::begin, src/codec.cpp)
        ▼
  ADC @ 44.1 kHz
        │
        ▼  (I²S RX FIFO)
  timecode_in task (core 0)       ← drains RX, feeds decoder
        │
        ▼
  timecode::Decoder               ← ZC-based speed + 20-bit LFSR bit-lock
        │
        ▼
  driveFromTimecode (main loop)   ← player::setSpeed + player::seekToMs
        │
        ▼
  player.cpp                      ← StreamCopy → MP3/WAV decoder → WSOLA → I²S TX
        │
        ▼
  ES8388 DAC → LOUT1 (HP jack) / LOUT2 (line out)
```

Two CPU cores:
- **Core 0**: tc decode (`timecode_in::taskEntry`), SD I/O when local-loop
  is on.
- **Core 1**: Arduino `loop()` — audio copier, WSOLA, `driveFromTimecode`,
  serial, controls.

Moving decode off core 1 was the fix for "tc decode distorts playback"
(commit 3941fb0).

---

## Why Keylock and not Pitched

The firmware has two speed-adjustment modes in `player.cpp`:
- **Pitched**: speed changes by retuning the I²S clock
  (`kit().setAudioInfo({sample_rate = nativeRate × speed})`). Classic
  turntable feel — pitch follows speed.
- **Keylock (WSOLA)**: speed changes by time-stretching in DSP
  (`wsolaStage.setSpeed`). I²S clock stays at 44.1 kHz. Pitch preserved.

**For timecode control, you must be in Keylock.** The ES8388 shares
its LRCK/BCLK lines between ADC and DAC, so re-clocking for Pitched
playback also re-clocks the ADC. That breaks the decoder's assumption
that 1 kHz carrier = 1× playback, because after a speed change the
decoder sees the same proportion of samples-per-carrier-cycle as
before (both rates scaled together). The decoder always reads 1.0×,
`driveFromTimecode` sees 1.0×, player holds — effectively a frozen
feedback loop.

Keylock avoids this by leaving I²S SR fixed. Decoder sees the
turntable's real speed.

`K` toggles Keylock mode. It only takes effect on the next `play()`.
Pitched-mode-with-timecode is a deferred open issue; the fix is
either independent ADC/DAC dividers on the codec (which ES8388
supports but the Arduino audio-tools layer doesn't expose cleanly)
or a TX-side software resampler that keeps I²S SR fixed.

---

## DAC→ADC crosstalk

Investigated in detail across the 2026-04-23 session.

**Symptom**: music playing on the DAC leaks into the ADC at roughly
unity gain after the input PGA. At output vol=100 and PGA=+24 dB,
the `rx` peak rails at 32767 regardless of whether the turntable is
even connected. That drowns the real timecode signal and breaks
`locked=1`.

**Ruled out**:
- Register misconfig by `play()`. Full reg dumps at idle vs playing
  vs stopped are byte-identical. `play()` writes zero codec registers.
- Specific output amp. Disabling LOUT2/ROUT2 (line out) or
  LOUT1/ROUT1 (HP) individually doesn't change rx during playback.
  Both paths couple.

**Confirmed**:
- DAC output drive is the source. `rx` peak scales with `setVolume()`:
  100 → 32767, 50 → ~200, 0 → ~150 baseline.
- Pause (`kit().setMute`) drops `rx` to baseline after ~1 s of DAC
  pipeline drain.
- Digital DAC mute (LDACVOL/RDACVOL = -96 dB via the `M` probe key)
  drops `rx` to baseline after ~3 s (internal ramp).

**Mitigation**: cap default output volume at 50 (see
`codec::DEFAULT_VOLUME`, `src/codec.cpp`). At vol=50 the coupled
amplitude stays below the turntable's ~21k peak, leaving usable SNR
for the decoder. The player output is loud enough for monitoring;
real DJ-level gain is expected to come from an external mixer or amp.
PGA default also drops from +24 dB → 0 dB (`DEFAULT_INPUT_GAIN`),
because +24 dB railed our current turntable input on its own.

A future fix is echo cancellation: tap TX samples, subtract a
scaled + delayed copy from RX before decoding. Preserves full DAC
output level. ~100 LOC, calibration required. Deferred as long as
the vol=50 default is acceptable.

---

## Position LUT on ESP32

The decoder's `position()` is a cycle index into the timecode
(0..712k for Vinyl, 0..950k for CD). Derivation: at every bit
boundary, look up the current 20-bit LFSR state in a table mapping
state → cycle index. The state space is 2²⁰ = 1 M entries.

### Packed 3-byte layout

```
g_devLut_ : uint8_t[1 048 576 × 3]      = 3 MiB in PSRAM
  entry at [state]  = 3 little-endian bytes
  cycle index fits in 20 bits (max 950k < 2²⁰)
  sentinel 0xFFFFFF = "state not reached in this format's sequence"
```

Why packed and not `uint32_t[1M]` (4 MB dense): the A1S V2.3's 4 MB
PSRAM has ~2 KB of heap overhead, so a single 4 MB allocation fails.
3 MB fits with ~900 KB headroom.

### Build + rebuild

- `timecode::prebuildLut(f)` — called from `timecode_in::begin()`
  during `setup()`. ~1.8 s for Vinyl (712k iterations), 2.5 s for CD.
  One-time stall at boot; without it the first `lookupPosition`
  stalls the tc task for that long.
- `timecode::rebuildLutInPlace(f)` — for runtime format switch
  (`F` key). Reuses the same 3 MB buffer — allocating a second LUT
  doesn't fit on a 4 MB board. `timecode_in::cycleFormat` suspends
  the tc task around the rewrite (`vTaskSuspend` / `vTaskResume`),
  and `dec_.begin()` resets decoder state inside the window so any
  partial lookup is discarded.
- Format switch leaks zero memory (same buffer) and works both
  directions.

### PSRAM requirement

`platformio.ini` sets `-DBOARD_HAS_PSRAM -mfix-esp32-psram-cache-issue`.
Without PSRAM detected (`ESP.getPsramSize() == 0`), the feature
degrades to "position always returns -1" with a loud serial warning;
speed control still works. Boot log prints PSRAM total + free for
confirmation.

---

## Track duration, seek, and mapping

### Duration parsing (`player.cpp:latchTrackDuration`)

Called from `play()` before the decoder pipeline starts.

**WAV**: read first 512 bytes, scan for `fmt ` and `data` chunk markers.
Non-44-byte headers are common (Dan's `house.wav` has `data@122`) —
the parser handles them. Duration = `(file_size - dataStart) × 1000
/ (SR × frame_bytes)`. Sample-accurate.

**MP3**: skip an ID3v2 tag if present (synchsafe 28-bit size field),
scan forward for the first `0xFF 0xEx` frame sync, decode bitrate
from the MPEG-1 Layer III frame header, compute
`duration = audio_bytes × 8 × 1000 / bitrate_bps`. CBR-accurate;
drifts on VBR content — tracked as a limitation.

### `player::seekToMs(uint32_t ms)`

Converts `ms` to a byte offset using the cached header info, calls
`audioFile.seek()`, resets `wsolaStage`. Returns `false` for `ms >=
track_dur`. WAV seeks are sample-accurate; MP3 seeks land on an
arbitrary byte and the decoder re-syncs on the next `0xFFE` frame
header, producing a brief click.

`S` key manually seeks to the track midpoint.

### Mapping modes

```cpp
enum class TransportMode { Absolute, Proportional };
```

- **Absolute** (default): `target_ms = tc_ms`. 1 ms of timecode =
  1 ms of track. Classic Serato/Traktor DVS feel. Short track on
  long vinyl: past-end policy kicks in when `tc_ms > track_dur`
  (player stops at end).
- **Proportional**: `target_ms = tc_ms × track_dur / vinyl_len`.
  Full vinyl range maps to full track range. A 3-min track covers
  the whole 12-min vinyl at 0.25× compression.

`P` toggles between them at runtime.

`mapTcToTrackMs(tc_ms, track_dur, vinyl_len, mode)` is the pure
helper; lives in `main.cpp`, easy to reason about separately from
the control loop.

---

## `driveFromTimecode` control loop

The function that ties decoder output to player action. Runs once
per `loop()` iteration. Structure:

```
1. Speed block
   - EMA-smooth the decoder's speed magnitude (α=0.15).
   - In Proportional mode, multiply by (track_dur / vinyl_len).
   - If |smoothed - lastApplied| >= 1% and >=50 ms since last apply:
        player.setSpeed(effective)
   - If decoder has reported "insane" values for 500 ms:
        revert player to 1.0×

2. Lock hysteresis
   - locked=1 now:   lastLockedMs = now; if paused-by-tc, unpause
   - locked=0 for >400 ms: pause the player, mark pausedByTc
   - locked=0: return — no position logic without lock

3. Position seek
   - tc_ms = position_cycles × 1000 / resolutionHz
   - target_ms = mapTcToTrackMs(tc_ms, track_dur, vinyl_len, mode)
   - target >= track_dur:  player.stop()   (past-end)
   - |cur - target| >= 300 ms and >=250 ms since last seek:
        player.seekToMs(target)
```

Key thresholds live as `constexpr` at the top of the function for
easy tuning. Rationale:
- **1% speed delta gate**: raw decoder jitter is ~1% per report
  window on a clean lock; below that we're chasing noise.
- **50 ms min speed interval**: caps WSOLA re-setup to 20 Hz.
- **500 ms insane-hold**: blips in signal shouldn't reset speed; a
  sustained dropout should.
- **400 ms unlock-to-pause**: brief lock blips during platter
  spin-up shouldn't pause; sustained unlock = needle up.
- **300 ms seek drift**: below ~1 beat, above the combined jitter
  of decoder + player-ms + control-loop delay. Steady playback
  stays below this threshold naturally.
- **250 ms min seek interval**: rate-limits seeks so we don't
  thrash WSOLA on near-threshold drift.

---

## Local-loop diagnostic mode

`timecode_in::setLocalLoop(true)` — `L` key. Decoder is fed PCM
frames from `/timecode.wav` on the SD card instead of the ADC.
Bypasses ADC, bypasses DAC, bypasses WSOLA (frames go directly to
`dec_.pushFrames`). Deterministic: steady speed≈1.0×, position
walks monotonically from 0.

Real-time pacing: file reads are near-instant but the decoder's ZC
timing is sample-rate dependent, so the task waits until
`next_batch_time_us = prev + frames × 10⁶ / SR` between batches.

Use it to:
- Validate decoder position and speed math without hardware.
- Exercise `driveFromTimecode` behavior without waiting on the
  turntable.
- A/B control-loop parameter changes against a known-stable signal.

Because `/timecode.wav` is CD format, press `F` first to switch the
decoder to CD. The default flag set is different per format
(`SWITCH_PRIMARY` for Vinyl + ES8388, no flags for CD + file) —
baked in via `timecode_in::cycleFormat`.

---

## Runtime key reference

Type `?` at the serial monitor for an abbreviated version. Here's
the comprehensive list.

### Browser screen
| Key       | Action |
|-----------|--------|
| `w` / `k` | Scroll up |
| `s` / `j` | Scroll down |
| Enter     | Play selected track |

### Playing screen
| Key | Action |
|-----|--------|
| `+` / `-` | Speed ±SPEED_STEP (fine, ~2%) |
| `]` / `[` | Speed ±0.1× (coarse) |
| `=`       | Snap speed to 1.0× |
| `<` / `>` | Hold to nudge ±2% (releases on key-repeat gap) |
| `p` / Space | Pause / resume |
| `b`       | Back to browser |
| `c`       | Jump to cue point (byte offset) / restart if no cue |
| `C`       | Set cue point to current byte position |

### Global
| Key | Action |
|-----|--------|
| `?`       | Print help |
| `r`       | Rescan SD for audio files |
| `K`       | Toggle Keylock mode (takes effect next play) |
| `t`       | Arm / disarm timecode decoder |
| `f`       | Cycle decoder flags (PHASE / PRIMARY / POLARITY, 0..7) |
| `F`       | Cycle decoder format (Vinyl ↔ CD, rebuilds LUT) |
| `L`       | Toggle local-loop (decoder fed from /timecode.wav, no ADC) |
| `P`       | Toggle transport mode (Absolute ↔ Proportional) |
| `S`       | Seek to track midpoint (manual `seekToMs` test) |
| `g` / `G` | Input PGA gain ± 3 dB |
| `v` / `V` | Output volume ± 10 (scales DAC output) |
| `D`       | Dump full ES8388 register table |

### Dev probes (kept in firmware for diagnostics)
| Key | Action |
|-----|--------|
| `1` | reg 0x04 = 0x0C (HP only, LOUT2/ROUT2 disabled) |
| `2` | reg 0x04 = 0x30 (LINE OUT only, LOUT1/ROUT1 off) |
| `3` | reg 0x04 = 0x3C (all outputs, default) |
| `M` | LDACVOL/RDACVOL = -96 dB (DAC digital mute) |
| `U` | LDACVOL/RDACVOL = 0 dB |

---

## Test harnesses (`tools/`)

All run via `~/.platformio/penv/bin/python tools/<name>.py`
(pyserial is bundled in the pio venv). They open `/dev/ttyUSB0`,
drive scripted keystrokes, and print timestamped output.

| Script | Purpose |
|--------|---------|
| `tc_probe.py <scenario>` | Multi-scenario probe; arg picks one: |
| &nbsp;&nbsp;`sine`     | Volume + pause + PGA ramps, separates pre- vs post-PGA coupling |
| &nbsp;&nbsp;`volume`   | Step DAC vol 100→0 during playback; confirms coupling is analog |
| &nbsp;&nbsp;`outputs`  | A/B LOUT1 vs LOUT2 to find dominant coupler (answer: neither) |
| &nbsp;&nbsp;`selfloop` | Play timecode.wav through DAC, decode via coupling |
| &nbsp;&nbsp;`smoothing`| Self-loop in keylock (limited — WSOLA mangles timecode) |
| &nbsp;&nbsp;`turntable`| End-to-end with real platter spinning |
| `tc_localloop.py`     | Phase 1 verification: L + F, expect locked=1 |
| `tc_flags_scan.py`    | Sweep all 8 decoder flag combos to find lock |
| `tc_duration.py`      | Phase 3 verification: WAV + MP3 duration parse |
| `tc_seek.py`          | Phase 4 verification: S key midpoint jump |
| `tc_phase6.py`        | Phase 6 end-to-end: ABS + PROP + needle-up pause |
| `tc_sanity.py`        | Quick sanity check: arm tc, read 6 s of traces |
| `tc_boot.py`          | RTS-based reset; captures boot log incl. PSRAM size |

---

## Known limitations

- **PSRAM required** for position feature. Without it, `position()`
  always returns -1 and the seek block in `driveFromTimecode`
  short-circuits. Speed control still works.
- **MP3 seek glitch**. Decoder re-syncs on next frame; brief click
  at each seek. A frame index would fix it (~80 KB/track scan at
  file open).
- **MP3 VBR duration**. Bitrate × filesize is off by 10-20% on VBR
  content; Proportional mapping drifts accordingly. Xing/Info
  header parse would fix.
- **Lock lag on needle drop**. ~260 ms from first bit to
  `locked=1` (decoder's `VALID_BITS=24` confirmation). First seek
  after a drop always feels a beat late.
- **Coupling still exists at vol > 50**. Drive the DAC harder and
  the coupling comes back; turntable signal gets drowned. External
  amp on the DJ mixer is the expected path.
- **Reverse scratch**. Player speed clamps positive; negative
  decoder speeds get |abs()|'d to prevent stuck-at-min. Real
  reverse-playback is deferred; needs signed-speed plumbing
  through player + WSOLA + I²S.
- **Pitched mode + timecode**. Feedback loop (see
  [Why Keylock](#why-keylock-and-not-pitched)). Must use Keylock.
