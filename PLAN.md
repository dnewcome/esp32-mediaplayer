# Timecode bring-up — next session

## Working at session end
- Decoder locks cleanly on real Serato Control Vinyl in isolation (platter spinning, no track playing): peak ~14–20k, `locked=1`, `speed≈1.0` on flag `0x2` (SWITCH_PRIMARY).
- Audio playback (pitched and keylock modes) works independently.
- `timecode_in` drain + decode run on core-0 RTOS task, off the main audio loop.
- Steering code (`driveFromTimecode`) updates player speed from timecode speed estimate; now gated on speed validity rather than bit-lock.

## Closed: "lock worked earlier, broke later at same firmware"
- Root cause: **turntable batteries were dying.** Output amplitude and SNR degraded as the supply voltage sagged, which moved the signal in and out of the decoder's usable window. Explains the drifting peak readings, the inconsistent lock, the "nothing changed but behavior changed" hours, and why the same commit behaved differently at the start vs. end of the session.
- Lesson: when symptoms drift without obvious cause on a battery-powered signal source, **check the source's power supply before assuming firmware/codec regression.**
- Keep the signal-level diagnostics (`[tc] rx=`, register dump) — they made the window-dependent behavior visible even if we didn't read it correctly.

## Uncommitted work to decide on
In working tree, not yet committed:
- `codec.cpp` — removed default `setInputVolume(0)` (so PGA stays at library default +24 dB, matching the `68d939d` working state). `adjustInputGain` starts `vol=100` to match reality.
- `main.cpp` — `driveFromTimecode` rewritten: gates on speed magnitude in 0.1–2.0 range instead of `locked=1`; 20 ms throttle on setSpeed calls (avoids I²S reclock stutter); auto-revert to 1.0× after 500 ms of insane/missing signal.
- `player.cpp` — reverted the TX meter routing change; `wsolaStage` sinks to `kit()` directly, `mp3ToKit`/`wavToKit` target `&kit()`. The VolumeMeter routing broke pitched-mode varispeed.
- `timecode_in.cpp` — reverted decoder-feed path to `68d939d` exactly (no portMUX critical section, volatile-only stats counters). Didn't change any symptom — keep or drop as we wish; the race is real but the fix wasn't load-bearing.

Commit in this order (split by concern):
1. `timecode steering: gate on speed validity, throttle reclocks` (main.cpp + player.cpp revert)
2. `codec: leave PGA at library default; add runtime adjust keys` (codec.cpp gain adjust default)

Decide tomorrow whether to re-commit the portMUX stats fix. If not kept, delete the `/tmp/timecode_in_current.cpp` backup.

## Phase-2 scope (all deferred)
- Transport control from timecode (needle-drop → auto-start, signal-loss → auto-stop)
- Reverse scratch — player's `setSpeed` clamps to ≥0.5; needs negative-speed plumbing through I²S clock and WSOLA
- Position LUT on ESP32 — currently host-only; needs PSRAM-resident compact form
- Bit-decode robustness at low signal levels — ref_level EMA and bit-sample threshold tuning

## Hardware-chain facts (don't re-discover)
- ADC must be routed to `ADC_INPUT_LINE2` (library default LINE1 is on-board mic, ignores the jack)
- Decoder needs `SWITCH_PRIMARY` flag — ES8388 delivers I²S frames with channels in opposite L/R order from xwax's primary=R convention
- Default format: `SeratoControlVinyl` (CD format has different LFSR, bit-lock impossible cross-format)
- ES8388 I²C addr 0x10, SCL=23 SDA=18
- Register map: `0x09` = mic PGA, `0x0A` = input select, `0x19` = DAC mute (changes on pause), `0x26/0x29` = output mixer routing
- PGA steps are 0/3/6/9/12/15/18/21/24 dB — no sub-3 dB granularity
- `kit().setVolume()` expects 0.0..1.0 float, not 0..100 int — library will accept int but it scales samples that much

## Ruled out (don't chase again)
- DAC→ADC analog crosstalk / bleed: the single-register diff (only `0x19` changes between pre-play and playing) plus the independent-TX-peak experiment showed DAC output level doesn't drive ADC peak. Elevated `rx` during playback in earlier sessions was either turntable-volume drift or measurement artifacts.
- Line-in→output mixer monitor: DACCONTROL17/20 bits checked; disabling them killed audio entirely, confirming those bits carry DAC to output, not line-in-to-output monitor.
