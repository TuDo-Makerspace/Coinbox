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

import os
import re
import sys


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
RESET = "\033[0m"
STYLES = {
    "bold": "\033[1m",
    "dim": "\033[2m",
    "red": "\033[31m",
    "green": "\033[32m",
    "yellow": "\033[33m",
    "blue": "\033[34m",
    "magenta": "\033[35m",
    "cyan": "\033[36m",
}


def colors_enabled(stream=None) -> bool:
    stream = stream or sys.stdout

    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("FORCE_COLOR") or os.environ.get("CLICOLOR_FORCE") not in (None, "", "0"):
        return True
    if os.environ.get("TERM", "") == "dumb":
        return False
    return bool(getattr(stream, "isatty", lambda: False)())


def style(text: str, *names: str, enabled: bool | None = None) -> str:
    if enabled is None:
        enabled = colors_enabled()
    if not enabled:
        return text

    prefix = "".join(STYLES[name] for name in names if name in STYLES)
    if not prefix:
        return text
    return f"{prefix}{text}{RESET}"


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def color_mode(stream=None) -> str:
    return "yes" if colors_enabled(stream=stream) else "no"


def status_style(status: str, enabled: bool | None = None) -> str:
    normalized = status.strip().lower()
    if normalized in ("passed", "pass"):
        return style(status, "green", "bold", enabled=enabled)
    if normalized in ("failed", "fail", "error", "errors"):
        return style(status, "red", "bold", enabled=enabled)
    if normalized in ("skipped", "skip"):
        return style(status, "yellow", "bold", enabled=enabled)
    return style(status, "cyan", enabled=enabled)
