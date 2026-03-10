# Playback Memory Mock

On the real ESP32, MP3 playback uses a noticeable amount of heap.

That matters because the settings page currently builds the full HTML page in RAM before sending it. The page needs about `102930` bytes in one contiguous allocation. During playback, that large allocation can fail, which shows up as:

- `500 Out of memory` on `/settings`

QEMU used to be too forgiving here. The mock playback path kept the playback state alive, but it did not consume heap in a way that looked like the real device. That meant some memory-pressure bugs were invisible in QEMU.

## Measurements

From one real-device capture:

- Before playback task start:
  - `free8=163228`
  - `largest8=110592`
- At peak playback (`decoder-info`):
  - `free8=76144`
  - `largest8=69632`

In simple terms:

- playback used about `87 KB` of normal heap
- the biggest free block dropped to about `69 KB`
- the playback task stack itself was not the problem
  - `4096` byte stack
  - about `1432` bytes still free at worst

## Requirements of the `/settings` page

The settings page render path allocates roughly:

- `settings_html_size + 512 + 1`
- in our case about `102930` bytes

During active playback, the real device only had about `69632` bytes as its largest free block. That is much smaller than the settings page needs, so the page render fails.

## Mocking the memory usage in the QEMU build

The timed mock playback path is the one used when the filename contains a duration marker such as:

- `test6165ms.mp3`

For that path, the mock now reserves heap while playback is active.

It does three things:

1. It targets a remaining free heap close to the real device.
2. It reserves one large block first to shrink the largest free block.
3. It then reserves several smaller blocks to imitate the many allocations the real audio pipeline makes.

A single big allocation is not enough. The real playback path uses many buffers, ringbuffers, task stacks, and helper allocations, so fragmentation matters.

## Current target behavior in QEMU

The mock is tuned so that timed mock playback ends up near this range:

- `free8` around `76-78 KB`
- `largest8` around `69 KB`

That is intentionally close to the hardware numbers above.

With the current tuning, QEMU now reproduces the real symptom:

- timed mock playback stays active
- `/audio/playback` still reports the correct active file
- `/settings` can return `500 Out of memory` during playback

## Where it lives

The implementation is in:

- `main/audio.c`

Look for the mock-only timed playback section guarded by:

- `CONFIG_TEST_AUDIO_MOCK_BACKEND`

The heap reservation is only applied to the timed mock playback path, not to normal hardware playback.

## Limits

This is still an approximation.

It does not recreate the exact internal allocations of the ADF MP3 pipeline. Instead, it recreates the important external effect:

- playback consumes a lot of heap
- the largest free block becomes too small for the settings page allocation

That is good enough for catching the class of bug we saw on hardware.
