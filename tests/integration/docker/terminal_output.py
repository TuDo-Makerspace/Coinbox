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
