# Audio Module Guide (`main/audio.c`)

The module has two jobs:

1. Play MP3 files from local storage.
2. Generate a test sine tone for maintenance.

It does both jobs through the same I2S output hardware, so it uses a mode system to avoid conflicts.

## Quick API Map

Main public functions are declared in `main/audio.h`.

- `audio_init()`
- `audio_mode()`
- `audio_stop()`
- `audio_start_file(const char *name)`
- `audio_is_playing()`
- `audio_test_start(float freq_hz, uint16_t amplitude)`
- `audio_test_set_targets(float freq_hz, uint16_t amplitude)`
- `audio_test_is_running()`
- `audio_set_master_volume_level(uint8_t level)`
- `audio_set_track_volume_level(uint8_t level)`
- `audio_set_lid_open_volume_level(uint8_t level)`
- `audio_set_lid_level(bool lid_open)`

## High-Level Design

The module keeps internal global state and runs background tasks:

- One playback task (`play_task`) for MP3 pipeline playback.
- One tone task (`tone_task`) for sine wave test output.
- A mutex (`s_lock`) to protect critical mode switches and startup/shutdown paths.

Only one mode should be active at a time:

- `AUDIO_MODE_PLAYBACK`
- `AUDIO_MODE_TEST`
- `AUDIO_MODE_UNINITIALIZED`

## Startup Flow

`audio_init()` does the setup:

1. Reads `CONFIG_BASE_PATH` (where files are stored, normally `/data`).
2. Creates the module mutex if needed.
3. Mutes DAC and amplifier first (safe startup).
4. Marks module initialized.
5. Switches default mode to playback by calling `switch_mode_locked(AUDIO_MODE_PLAYBACK)`.

Important detail: startup always begins muted to reduce pop/click noise.

## Mode Switching

`switch_mode_locked()` is the core transition function.

When switching to playback mode:

1. Stops tone task.
2. Deconfigures test I2S driver.
3. Initializes playback I2S stream (ADF `i2s_stream`).
4. Mutes outputs.
5. Sets mode to `AUDIO_MODE_PLAYBACK`.

When switching to test mode:

1. Stops playback task.
2. Destroys playback pipeline (keeps/deinits stream as needed).
3. Deinitializes playback I2S stream.
4. Configures raw I2S driver for tone generation.
5. Mutes outputs.
6. Sets mode to `AUDIO_MODE_TEST`.

This prevents two code paths from driving I2S at the same time.

## Output Muting Strategy

There are two mute control lines:

- DAC mute: board-level control in `audio_board_outputs_*()`
- AMP mute: board-level control in `audio_board_outputs_*()`

`mute_outputs()` turns both off immediately.

`unmute_outputs()` enables them in sequence with short delays:

1. Unmute amplifier.
2. Wait 20 ms.
3. Unmute DAC.
4. Wait 30 ms.

This staged unmute helps reduce audible pops.

## Playback Mode in Detail

### 1) Starting a file

`audio_start_file(name)`:

1. Validates input:
   - Name must not be empty.
   - Rejects `/` and `..` for path safety.
2. Locks audio state.
3. Verifies module initialized.
4. Switches to playback mode.
5. Stops any old playback task.
6. Builds full path as `"<base_path>/<name>"`.
7. Checks file exists and has size (`stat`).
8. Tries to read metadata sidecar (`files_read_meta`), but this is optional.
9. Creates `play_task`.

If task creation succeeds, playback begins asynchronously.

### 2) Playback pipeline setup

Inside `play_task`, it builds an ESP-ADF pipeline:

1. `fatfs_stream` reader (`file`)
2. `mp3_decoder` (`mp3`)
3. `i2s_stream` writer (`i2s`)

Link order: `file -> mp3 -> i2s`

Then it:

1. Sets URI to the selected file path.
2. Sets element info (byte position and total bytes).
3. Creates event interface listener.
4. Runs pipeline.

### 3) Playback loop behavior

The task loop waits on audio events.

On timeout:

- Applies volume updates if needed (`s_volume_dirty`).
- Unmutes after a small startup delay (about 60 ms), if not already unmuted.

On `AEL_MSG_CMD_REPORT_MUSIC_INFO` from decoder:

1. Reads real decoded stream info (sample rate, bits, channels).
2. Applies this format to I2S stream.
3. Sets I2S clock accordingly.
4. Applies volume.
5. Unmutes if still muted.

On finished/stopped status:

- Breaks loop and exits cleanly.

### 4) Playback stop and cleanup

When exiting `play_task`:

1. Optional fade-out.
2. Mute outputs.
3. Destroy active pipeline objects.
4. Clear task/running flags.
5. Delete task.

External stop is done by `audio_stop()` or by starting a new mode/file.

## Test Mode in Detail

### 1) Test tone parameters

Test mode uses:

- Sample rate: `44100 Hz`
- Frequency range clamp: `50 Hz` to `8000 Hz`
- Default frequency: `440 Hz`
- Amplitude max: `CONFIG_AUDIO_MAX_AMPLITUDE` (default `0x30`)

`audio_test_set_targets(freq, amp)` clamps and stores targets.

### 2) Test I2S setup

`configure_test_i2s_locked()` installs raw I2S driver (`I2S_NUM_0`) in TX mode:

- 16-bit stereo
- DMA buffers configured for continuous tone writes
- Pin assignment:
  - BCK: GPIO19
  - WS: GPIO18
  - DATA: GPIO17

### 3) Tone generation task

`tone_task` loop:

1. Reads current target frequency/amplitude.
2. Fills a stereo sample block using `sinf` and a phase accumulator.
3. Writes block with `i2s_write`.
4. Repeats until stop is requested.

When stopping:

1. Mutes outputs.
2. Zeros DMA buffer.
3. Clears task handle and flags.

### 4) Starting/stopping test mode

`audio_test_start(freq, amp)`:

1. Locks module.
2. Verifies initialized.
3. Stores clamped targets.
4. Switches to test mode.
5. Starts tone task if not already running.
6. Unmutes outputs.

If already running, it simply updates targets and keeps output active.

## Volume System

Playback volume is the product of three percentages:

- Master volume (`s_master_volume`)
- Track volume (`s_track_volume`)
- Lid-open volume factor (`s_lid_open_volume`) when lid is open

Formula used:

1. `effective_pct = master * track * lid_factor * 100`
2. `db = -60 + 0.6 * effective_pct`
3. Clamp dB to `[-80, 0]`

Notes:

- Volume changes are marked dirty and applied in playback loop.
- Test tone loudness is mainly controlled by sine amplitude, not playback dB path.

## Fade-Out Behavior

Before muting on playback stop, `fade_out_then_mute()` can ramp down volume:

- 6 small steps
- ~8 ms between steps
- Then hard mute

This is controlled by `AUDIO_ENABLE_FADE_OUT` (enabled by default).

## Concurrency and Safety

- Mode-changing operations use `s_lock`.
- Stop requests are done with flags (`s_play_stop_requested`, `s_test_stop_requested`).
- Stop helpers wait briefly for task handles to clear.
- Filenames are sanitized to reduce path traversal risk.

## File and Metadata Handling

For playback, the module:

1. Requires the file to exist and have non-zero size (`stat`).
2. Attempts metadata load (`files_read_meta`), but playback still works if metadata is missing.

So metadata is optional; the file itself is mandatory.

## Common Call Flows

### Normal startup and playback

1. `audio_init()`
2. `audio_start_file("some.mp3")`
3. Optional: adjust volume levels while playing.
4. `audio_stop()` when needed.

### Maintenance tone

1. `audio_init()` (once)
2. `audio_test_start(440.0f, audio_test_max_amplitude())`
3. Optional live updates: `audio_test_set_targets(...)`
4. `audio_stop()` to silence output.

## Configuration Inputs (Kconfig)

From `main/Kconfig.projbuild`:

- `CONFIG_BASE_PATH`
  - Root path for audio files.
- `CONFIG_AUDIO_MAX_AMPLITUDE`
  - Max test tone amplitude.

These values directly affect runtime behavior in `audio.c`.

## Practical Notes

- The module is designed for one active output path at a time.
- Playback and test mode both use I2S0 and shared mute controls.
- If playback appears silent, check:
  - mute GPIO logic,
  - volume levels,
  - file validity and path,
  - I2S pin wiring (GPIO17/18/19).
