#!/usr/bin/env python3
"""
mp3tosample.py
-------------------
- Removes leading / trailing silence
- Hard-trims (or pads) the result to ≤5 s
- Peak-normalises to -0.5 dBFS
- Down-mixes to mono, resamples to 16 kHz
- Saves as **8-bit unsigned PCM** WAV
Usage:  python mp3tosample.py  input.mp3  output.wav
"""

from pathlib import Path
import sys, math, tempfile, subprocess
from pydub import AudioSegment, silence    # pip install pydub
                                          # ffmpeg must be on your PATH

# ------------------ tweakables ------------------
SILENCE_THRESH  = -50        # dBFS.  Higher = less aggressive trimming
CHUNK_MS        = 10         # Analysis granularity for silence detect
TARGET_PEAK_DB  = -0.5       # Desired peak after normalisation
MAX_LENGTH_MS   = 5_000      # 5 seconds
# ------------------------------------------------

def strip_silence(seg):
    lead = silence.detect_leading_silence(seg,
                                           silence_threshold=SILENCE_THRESH,
                                           chunk_size=CHUNK_MS)
    tail = silence.detect_leading_silence(seg.reverse(),
                                           silence_threshold=SILENCE_THRESH,
                                           chunk_size=CHUNK_MS)
    return seg[lead:len(seg)-tail]

def main(inp, out):
    audio = AudioSegment.from_file(inp)

    # 1.  Trim silence
    audio = strip_silence(audio)

    # 2.  Trim to 5 s
    if len(audio) > MAX_LENGTH_MS:
        audio = audio[:MAX_LENGTH_MS]

    # 3.  Peak-normalise to -0.5 dB
    change = TARGET_PEAK_DB - audio.max_dBFS   # dB to add (may be + or –)
    audio = audio.apply_gain(change)

    # 4.  Format conversion: mono, 16 kHz, 8-bit unsigned PCM
    audio = (audio.set_frame_rate(16_000)
                  .set_channels(1)
                  .set_sample_width(1))         # 1 byte = 8-bit

    # 5.  Export.  pydub hands off to ffmpeg; the extra parameter forces pcm_u8
    audio.export(out, format="wav",
                 parameters=["-acodec", "pcm_u8"])

    print(f"Wrote {out} ({len(audio)/1000:.2f}s, "
          f"{audio.frame_rate} Hz, {audio.sample_width*8}-bit unsigned)")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("Usage: mp3tosample.py  input.mp3  output.wav")
    main(Path(sys.argv[1]), Path(sys.argv[2]))
