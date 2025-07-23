#!/usr/bin/env python3
"""
wav_to_header.py – Convert an 8-bit unsigned PCM WAV file to a self-contained
C header (array **without** a hard-coded length).

Example
-------
$ python wav_to_header.py welcome.wav          # → welcome.h, array "sample[]"
$ python wav_to_header.py welcome.wav -n beep  # → welcome.h, array "beep[]"

The header now ends with:
    const unsigned char sample[] = { 0x52, 0x49, ... };
    const unsigned int  sample_len = sizeof(sample);  /* compile-time length */

Why?  ☞  The array length is always correct, even if you regenerate the header
with a different file, and you have an easy symbol to pass to playback code.
"""
import argparse
import os
import textwrap
import wave
from typing import List

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _read_all_bytes(path: str) -> bytes:
    """Return the entire file as bytes."""
    with open(path, "rb") as fp:
        return fp.read()


def _ensure_8bit_pcm(path: str) -> None:
    """Verify *path* is an 8-bit unsigned PCM WAV; else raise ValueError."""
    try:
        with wave.open(path, "rb") as w:
            if w.getsampwidth() != 1:
                raise ValueError(
                    f"{os.path.basename(path)} is {w.getsampwidth()*8}-bit; expected 8-bit"
                )
            if w.getcomptype() != "NONE":
                raise ValueError(
                    f"{os.path.basename(path)} is compressed ({w.getcomptype()}); expected PCM"
                )
    except wave.Error as exc:
        raise ValueError(f"Invalid WAV file: {exc}") from exc


def _fmt_c_bytes(data: bytes, per_line: int) -> str:
    """Return *data* as comma-separated hex bytes, *per_line* per row."""
    lines: List[str] = []
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        hexes = ", ".join(f"0x{b:02X}" for b in chunk)
        comma = "," if i + per_line < len(data) else ""  # no comma on last line
        lines.append(f"    {hexes}{comma}")
    return "\n".join(lines)


def _make_header(data: bytes, *, src: str, arr: str, width: int) -> str:
    """Assemble the C header string."""
    size = len(data)
    head = textwrap.dedent(
        f"""
        //------------------------------------------------------------
        //-----------      Created with wav_to_header.py       -------
        //
        // File    : {src}
        // Address : 0 (0x0)
        // Size    : {size} (0x{size:X})
        //------------------------------------------------------------
        const unsigned char {arr}[] = {{
        """
    ).lstrip()

    body = _fmt_c_bytes(data, width)
    tail = textwrap.dedent(
        f"""
        }};
        const unsigned int {arr}_len = sizeof({arr});
        """
    )
    return head + body + tail


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Convert an 8-bit unsigned PCM WAV to a C header (no fixed length)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("input", help="Path to 8-bit unsigned PCM .wav file")
    ap.add_argument("-o", "--output", help="Header output (default: <input>.h)")
    ap.add_argument("-n", "--name", default="sample", help="C array base name")
    ap.add_argument("-w", "--width", type=int, default=16, help="Hex values per line")
    args = ap.parse_args()

    _ensure_8bit_pcm(args.input)
    data = _read_all_bytes(args.input)

    hdr = _make_header(
        data,
        src=os.path.abspath(args.input),
        arr=args.name,
        width=args.width,
    )

    out = args.output or os.path.splitext(args.input)[0] + ".h"
    with open(out, "w", newline="\n") as f:
        f.write(hdr)
    print(f"{out} generated  (len={len(data)})")


if __name__ == "__main__":
    main()
