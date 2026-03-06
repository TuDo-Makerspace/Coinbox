# Coin-To-Sound Flow (Simple, End-to-End)

The short version:

1. Laser input changes state.
2. GPIO interrupt fires.
3. Interrupt wakes a worker task.
4. Worker picks a weighted sound file.
5. Audio module stops current playback (if any) and starts the new file.

The long version is below.

## 1) What gets initialized at boot

`app_main()` initializes modules in this order:

1. `files_init()` mounts storage.
2. `gpio_init()` configures laser/hall input pins and ISR handlers.
3. `audio_init()` prepares audio output and playback mode.

Relevant file:

- `main/main.c`

Why this matters:

- The laser event path depends on both GPIO and file metadata.
- Audio playback will only work once `audio_init()` has run.

## 2) Laser input and interrupt setup

In `gpio_init()`:

1. Laser pin is configured as input with rising-edge interrupt.
2. ISR service is installed.
3. Laser ISR (`gpio_laser_isr_handler`) is attached.
4. A worker task (`laser_event_task`) is created.

Relevant file:

- `main/gpio.c`

Important constants:

- Laser pin: `GPIO_LASER_RECEIVER = 23`
- Logical blocked level: `LASER_BLOCKED = 1` (from `main/gpio.h`)

## 3) What the ISR does (and does not do)

ISR work is intentionally tiny:

1. Increment ISR counter.
2. Notify `laser_event_task` using `xTaskNotifyFromISR(..., eSetValueWithOverwrite, ...)`.
3. Return immediately.

It does **not** do heavy work like file access or audio startup.

Why overwrite mode matters:

- `eSetValueWithOverwrite` means only the latest pending trigger is kept.
- If many fast triggers happen, old pending ones are replaced.
- This avoids the old FIFO backlog behavior.

Think of it like a single sticky note:

- New trigger replaces the note content.
- Worker always sees the most recent note, not a pile of old notes.

## 4) Worker task: `laser_event_task`

This is where the real logic runs.

Main loop:

1. Wait for a task notification.
2. Drain any immediate extra notifications and keep the latest one.
3. Optional debounce check.
4. Optional cooldown check.
5. Pick one sound file using weighted metadata.
6. Call `audio_start_file(selected_name)`.

### Debounce and cooldown

The code supports both, but currently they are disabled by config values:

- `s_debounce_time_laser_ms = 0`
- `s_laser_trigger_cooldown_ms = 0`

When non-zero:

- Debounce waits and re-reads the laser pin to reject quick bounces.
- Cooldown rate-limits retriggers.

### If no sound can be picked

If no file is eligible (for example all disabled or all weight=0), playback is skipped and a warning is logged.

## 5) Weighted sound selection

Selection happens in:

- `files_pick_weighted_enabled(...)` in `main/files.c`

Rules:

1. Iterate files in storage.
2. Ignore non-entry files and broken metadata entries.
3. Keep only files where:
   - `enabled == true`
   - `probability > 0` (UI label is “Weight”)
4. Use weighted random replacement so chance is proportional to weight.

Example:

- `soundA` weight 100
- `soundB` weight 100
- `soundC` weight 20

Result:

- A and B are equally likely.
- C is less likely.
- Multiple files at 100 is valid and expected.

## 6) Starting audio: `audio_start_file(name)`

`laser_event_task` calls `audio_start_file(selected_name)`.

Inside `audio_start_file`:

1. Validate filename (must be simple name, no slash or `..`).
2. Lock audio state mutex.
3. Ensure playback mode is active.
4. Stop current playback task if one is running.
5. Build full file path (`<base_path>/<name>`).
6. Load file stat and sidecar metadata.
7. Start a new playback task (`play_task`).

Relevant file:

- `main/audio.c`

## 7) What happens to an already-playing file

On a new trigger:

1. Current playback receives stop request.
2. Playback task exits its loop, fades/mutes outputs, tears down pipeline.
3. New file task starts.

So yes, new events interrupt/restart playback.

Why it may still feel like “not instant” sometimes:

1. Stop request is cooperative (task checks flag in loop).
2. There is still a short fade/mute path during shutdown.
3. Decoder/I2S pipeline needs a short setup before audible output.

To improve responsiveness, event-wait granularity in playback loop is set to:

- `AUDIO_PLAY_EVENT_WAIT_MS = 50`

This reduces stop reaction delay compared with a long wait interval.

## 8) Playback pipeline internals (high-level)

`play_task` creates and runs an ESP-ADF pipeline:

1. File reader (`fatfs_stream`)
2. MP3 decoder (`mp3_decoder`)
3. I2S writer (`i2s_stream`)

Then it:

1. Listens for decoder/status events.
2. Applies sample-rate/bit-depth/channel info to I2S.
3. Applies volume logic.
4. Exits on stop/finish/error.

## 9) Why this design uses ISR + task split

This pattern is used for safety and timing:

1. ISR stays tiny and deterministic.
2. Heavy work (filesystem, random selection, playback start) runs in task context.
3. The system remains responsive under frequent sensor activity.

## 10) Current “latest trigger wins” behavior

Today’s behavior is:

1. Do not queue old triggers.
2. Keep latest pending trigger only.
3. Restart playback from selected file when processed.

This directly addresses the old “work through a backlog of events” effect.

## 11) Practical tuning knobs

If behavior needs adjustment later, these are the main knobs:

1. `s_debounce_time_laser_ms` in `main/gpio.c`
2. `s_laser_trigger_cooldown_ms` in `main/gpio.c`
3. `AUDIO_PLAY_EVENT_WAIT_MS` in `main/audio.c`
4. Per-file `enabled` + `probability` metadata via Sounds UI

## 12) Sequence summary (mental model)

1. Beam changes -> GPIO edge.
2. ISR posts latest trigger to worker.
3. Worker validates trigger timing (debounce/cooldown if enabled).
4. Worker picks one weighted sound.
5. Audio module interrupts current playback (if needed).
6. New MP3 pipeline starts.
7. Sound is heard.

