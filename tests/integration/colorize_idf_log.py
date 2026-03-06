#!/usr/bin/env python3
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
