# ADF Audio Unification Writeup

## Why this document exists

This document explains, in simple language, what we changed in the audio subsystem and why.

The short version:

1. Playback was silent until test tone ran once.
2. Root cause was I2S pin ownership mismatch between ADF board profile and app wiring.
3. We created a minimal custom local ADF board profile for this hardware.
4. We moved test tone generation to ADF (`raw_stream`) so both playback and test use the same output stack.
5. That exposed a shared-gain side effect (quiet tone), which we fixed.

---

## Initial problem

### Symptom

- File playback produced no audible sound after boot.
- Running the audio test once "woke it up", then playback started working.
- You still heard a click/pop, so amp power/mute was not the full story.

### What this usually means

A click means the analog side is alive, but digital audio routing (I2S pin mapping or data path) may be wrong.

---

## Real root cause

## Two different I2S worlds were active

At the start of this work, the project had:

1. Playback path through ADF `i2s_stream`.
2. Test tone path through legacy IDF `i2s_driver_install + i2s_write`.

Both talked to I2S, but they did not get pin routing from the same source.

## Why ADF playback pins were wrong

On IDF5, ADF `i2s_stream` resolves GPIO from `get_i2s_pins(...)` (board profile).

- The selected ADF board config was a stock profile (LyraT), with different pin assumptions.
- Your actual board wiring is:
  - `BCK = GPIO19`
  - `WS = GPIO18`
  - `DOUT = GPIO17`

So playback sometimes started on wrong pins, while test tone explicitly set the correct pins.

Running test mode once reconfigured hardware state enough that playback appeared to recover.

---

## Design goal

Use one coherent source of truth for audio output routing and behavior:

1. Board profile defines I2S pins for this real hardware.
2. Playback and test use ADF path consistently.
3. Avoid hidden side effects from mixed legacy + ADF setup.

---

## What we changed

## 1) Added a project-local minimal ADF board profile

We added a local component override:

- `components/audio_board/...`

This intentionally replaces ADF's built-in `audio_board` component for this project only.

### Why local override

- No need to patch your global ESP-ADF checkout.
- Repo remains self-contained.
- Board behavior is explicit and versioned with your app.

### Files added

- `components/audio_board/CMakeLists.txt`
- `components/audio_board/include/board.h`
- `components/audio_board/include/board_def.h`
- `components/audio_board/include/board_pins_config.h` (copied interface header)
- `components/audio_board/board.c`
- `components/audio_board/board_pins_config.c`

### What this board profile does

- Provides required ADF `board.h` APIs so dependent components compile.
- Defines real I2S pins for your hardware:
  - `BCK=19`, `WS=18`, `DOUT=17`, no MCLK, no DIN
- Marks optional hardware as absent:
  - no SD card
  - no LEDs
  - no key/touch buttons
  - no headphone detect
  - no generic ADF `PA_ENABLE_GPIO` pin (coinbox uses explicit DAC/AMP mute control)

### About `audio_board_codec_init`

- We initialize an `audio_hal` handle via `ES7148` default handle in the minimal board.
- This is mainly to satisfy ADF expectations cleanly.
- Output mute control now also lives at board level in `components/audio_board/board.c`
  via `audio_board_outputs_init/mute/unmute`.

---

## 2) Removed temporary pin-force workaround from playback

Earlier, we had a temporary IDF5 workaround in `main/audio.c` that force-routed GPIO matrix outputs after `i2s_stream_init` / clock changes.

After the custom board profile was in place, that hack was no longer necessary.

Now playback gets the correct pins directly from the board profile.

---

## 3) Migrated test tone output to ADF using `raw_stream`

### Previous test mode

- Legacy IDF I2S driver install/uninstall in test mode.
- Raw `i2s_write()` of generated sine blocks.

### New test mode

Test uses ADF pipeline:

- `raw_stream (writer)` -> `i2s_stream (writer)`

And still generates the sine wave in code exactly as before.

So generation is unchanged, but transport/output now matches playback stack.

### Why `raw_stream` and not `tone_stream`

- `tone_stream` in ADF is for reading prebuilt files from flash partitions (`flash://tone/...`).
- Your test mode is runtime synthesis (frequency/amplitude adjustable in real time).
- Therefore `raw_stream` is the correct ADF tool.

---

## 4) Fixed quiet test tone after unification

After moving test to `raw_stream`, tone became much quieter.

### Root cause

Playback and test now share the same `s_playback_i2s_stream` handle.

That handle keeps ALC gain state. Playback volume policy had already set gain using:

- `master volume`
- `track volume`
- `lid-open scaling`

With default values (`35% * 35%`), effective dB was very low. Test inherited that attenuation.

### Fix

When entering test pipeline setup, we force stream gain to full scale:

- `set_stream_volume_db(AUDIO_MAX_DB, false)`

When returning to playback flow, playback volume policy gets applied again by normal logic.

### Extra note

Your test waveform max amplitude is also capped by config:

- `CONFIG_AUDIO_MAX_AMPLITUDE` in `sdkconfig`

So even at full stream gain, waveform headroom depends on this cap.

---

## Current architecture after changes

## Playback mode

- Source: filesystem MP3
- Pipeline: `fatfs_stream -> mp3_decoder -> i2s_stream`
- Pins: from local board profile (`get_i2s_pins`)
- Volume: playback policy (`master * track * lid` mapped to dB)

## Test mode

- Source: runtime sine generator in `tone_task`
- Pipeline: `raw_stream -> i2s_stream`
- Same physical I2S route as playback
- Gain forced to `0 dB` for predictable loudness

## Mode switching

Still centralized through `switch_mode_locked(...)`:

- Stops active task of previous mode
- Tears down mode-specific pipeline state
- Brings up target mode state
- Keeps mute/unmute sequencing controlled

---

## File-by-file summary

## `main/audio.c`

Major changes made:

1. Test mode no longer installs legacy I2S driver.
2. Added test ADF pipeline members:
   - `s_test_pipeline`
   - `s_test_raw_stream`
3. Added lifecycle helper:
   - `destroy_test_pipeline_locked()`
4. `configure_test_i2s_locked()` now:
   - initializes shared `i2s_stream`
   - creates/links/runs `raw -> i2s` pipeline
   - sets test stream gain to `AUDIO_MAX_DB`
5. `tone_task()` now writes with `raw_stream_write(...)`.

## `components/audio_board/include/board_def.h`

- Defines minimal board macros expected by ADF ecosystem.
- Declares absent peripherals as disabled/not present.

## `components/audio_board/board_pins_config.c`

- Implements `get_i2s_pins()` with real board wiring.
- Stubs out unsupported peripherals safely.

## `components/audio_board/board.c`

- Minimal board API implementation required by ADF components.
- Keeps unsupported features no-op or not supported.

---

## Verification done

- Rebuilt project successfully with `idf.py build` after each major step.
- Confirmed local `components/audio_board` is the selected component path.
- Confirmed playback and test now share ADF output path.
- Confirmed quiet-tone regression fixed by forcing test-mode stream gain.

---

## Why this is better now

1. One I2S pin definition path for the project.
2. No hidden behavior from mixing two different output stacks.
3. Easier debugging: both playback and test travel through ADF output.
4. Board details are in project repo, not spread into global ADF checkout.

---

## Remaining caveats and future cleanup

1. `main/audio.c` still includes legacy `driver/i2s.h` because test generation still uses some legacy types/APIs indirectly. It builds, but IDF prints deprecation warnings.
2. `docs/audio.md` and some diagrams still describe old test-mode internals (legacy `i2s_write` path). They should be refreshed.
3. If test loudness still feels low, adjust:
   - `CONFIG_AUDIO_MAX_AMPLITUDE`
   - analog gain/stage configuration in hardware

---

## Practical troubleshooting checklist (current design)

If playback/test audio is wrong again, check in this order:

1. `components/audio_board/board_pins_config.c` matches actual wiring.
2. `get_i2s_pins()` returns expected values for port 0.
3. Mute GPIO behavior in `main/gpio.c` (DAC mute polarity, AMP mute polarity).
4. Test stream gain line in `configure_test_i2s_locked()` still sets `AUDIO_MAX_DB`.
5. `CONFIG_AUDIO_MAX_AMPLITUDE` not set too small.

---

## One-line takeaway

The original issue was not a dead amp. It was split ownership of I2S setup. We fixed it by making ADF own both playback and test output with a board profile that matches your actual hardware.
