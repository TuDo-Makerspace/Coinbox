#!/usr/bin/env python3

# The MIT License (MIT)
#
# Copyright (c) 2026 TuDo Makerspace
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from __future__ import annotations

import re
import sys


RESET = "\x1b[0m"
RED = "\x1b[31m"
YELLOW = "\x1b[33m"
GREEN = "\x1b[32m"
CYAN = "\x1b[36m"
DIM = "\x1b[90m"

PREFIX_RE = re.compile(r"^([EWIDV]) \(\d+\)")
ERROR_RE = re.compile(r"\b(error|assert|panic|guru meditation|backtrace)\b", re.IGNORECASE)
WARN_RE = re.compile(r"\b(warn|warning)\b", re.IGNORECASE)
INFO_RE = re.compile(r"\b(info)\b", re.IGNORECASE)
DEBUG_RE = re.compile(r"\b(debug|trace)\b", re.IGNORECASE)


def _color_for_line(line: str) -> str | None:
    prefix = PREFIX_RE.match(line)
    if prefix:
        level = prefix.group(1)
        if level == "E":
            return RED
        if level == "W":
            return YELLOW
        if level == "I":
            return GREEN
        if level == "D":
            return CYAN
        if level == "V":
            return DIM

    if ERROR_RE.search(line):
        return RED
    if WARN_RE.search(line):
        return YELLOW
    if INFO_RE.search(line):
        return GREEN
    if DEBUG_RE.search(line):
        return CYAN
    return None


def main() -> int:
    try:
        for raw in sys.stdin:
            line = raw.rstrip("\n")
            color = _color_for_line(line)
            if color is None:
                sys.stdout.write(line + "\n")
            else:
                sys.stdout.write(f"{color}{line}{RESET}\n")
            sys.stdout.flush()
    except BrokenPipeError:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
