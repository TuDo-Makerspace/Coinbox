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

import argparse
import json
import sys
from pathlib import Path

from terminal_output import colors_enabled, status_style, style


def _load_summaries(results_root: Path) -> list[dict]:
    summaries = []
    for path in sorted(results_root.glob("*/summary.json")):
        try:
            summaries.append(json.loads(path.read_text(encoding="utf-8")))
        except json.JSONDecodeError:
            continue
    return summaries


def _format_duration(seconds: float) -> str:
    total = int(round(seconds))
    minutes, secs = divmod(total, 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        return f"{hours}h {minutes}m {secs}s"
    if minutes:
        return f"{minutes}m {secs}s"
    return f"{secs}s"


def _write_summary_text(path: Path, lines: list[str]):
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _print_colored_summary(results_root: Path, summaries: list[dict], any_failed: bool):
    color = colors_enabled()

    if not summaries:
        print(style("No integration test summaries were found.", "yellow", "bold", enabled=color))
        print(f"results_root: {style(str(results_root), 'dim', enabled=color)}")
        return

    started = [float(item.get("started_at_epoch_s", 0.0)) for item in summaries]
    ended = [float(item.get("ended_at_epoch_s", 0.0)) for item in summaries]
    wall_clock_s = max(ended) - min(started)

    total_services = len(summaries)
    passed_services = sum(1 for item in summaries if item.get("exit_code", 1) == 0)

    total_tests = 0
    total_passed = 0
    total_failed = 0
    total_errors = 0
    total_skipped = 0
    for item in summaries:
        counts = item.get("counts", {})
        total_tests += int(counts.get("total", 0))
        total_passed += int(counts.get("passed", 0))
        total_failed += int(counts.get("failed", 0))
        total_errors += int(counts.get("errors", 0))
        total_skipped += int(counts.get("skipped", 0))

    print(style("Integration Test Summary", "cyan", "bold", enabled=color))
    print(f"results_root: {style(str(results_root), 'dim', enabled=color)}")
    print(
        "services: "
        f"{style(str(passed_services), 'green', enabled=color)}/"
        f"{style(str(total_services), 'bold', enabled=color)} passed, "
        f"{style(str(total_services - passed_services), 'red' if any_failed else 'green', enabled=color)}/"
        f"{style(str(total_services), 'bold', enabled=color)} failed"
    )
    print(
        "tests: "
        f"{style(str(total_passed), 'green', enabled=color)} passed, "
        f"{style(str(total_failed), 'red', enabled=color)} failed, "
        f"{style(str(total_errors), 'red', enabled=color)} errors, "
        f"{style(str(total_skipped), 'yellow', enabled=color)} skipped, "
        f"{style(str(total_tests), 'bold', enabled=color)} total"
    )
    print(f"wall_clock: {style(_format_duration(wall_clock_s), 'magenta', enabled=color)}")
    print("")

    for item in summaries:
        counts = item.get("counts", {})
        service_name = str(item.get("service_name"))
        service_status = "PASSED" if item.get("exit_code", 1) == 0 else "FAILED"
        print(
            f"{style(service_name, 'bold', enabled=color)}: "
            f"{status_style(service_status, enabled=color)} "
            f"in {style(_format_duration(float(item.get('duration_s', 0.0))), 'magenta', enabled=color)}"
        )
        print(f"  target: {style(str(item.get('target')), 'blue', enabled=color)}")
        print(
            "  counts: "
            f"{style(str(counts.get('passed', 0)), 'green', enabled=color)} passed, "
            f"{style(str(counts.get('failed', 0)), 'red', enabled=color)} failed, "
            f"{style(str(counts.get('errors', 0)), 'red', enabled=color)} errors, "
            f"{style(str(counts.get('skipped', 0)), 'yellow', enabled=color)} skipped, "
            f"{style(str(counts.get('total', 0)), 'bold', enabled=color)} total"
        )
        print(f"  pytest log: {style(str(item.get('pytest_log')), 'dim', enabled=color)}")
        print(f"  qemu log: {style(str(item.get('qemu_log')), 'dim', enabled=color)}")
        if item.get("idf_py_stdout_log"):
            print(f"  idf stdout: {style(str(item.get('idf_py_stdout_log')), 'dim', enabled=color)}")
        if item.get("idf_py_stderr_log"):
            print(f"  idf stderr: {style(str(item.get('idf_py_stderr_log')), 'dim', enabled=color)}")
        for test in item.get("tests", []):
            test_status = str(test.get("status", "unknown"))
            line = f"  {status_style(test_status.upper(), enabled=color)}: {test.get('id')}"
            message = str(test.get("message", "")).strip()
            if message:
                line += f" :: {style(message, 'dim', enabled=color)}"
            print(line)
        print("")


def build_summary_lines(results_root: Path, summaries: list[dict]) -> tuple[list[str], bool]:
    lines: list[str] = []
    any_failed = any(item.get("exit_code", 1) != 0 for item in summaries)

    if not summaries:
        lines.append("No integration test summaries were found.")
        lines.append(f"results_root: {results_root}")
        return lines, True

    started = [float(item.get("started_at_epoch_s", 0.0)) for item in summaries]
    ended = [float(item.get("ended_at_epoch_s", 0.0)) for item in summaries]
    wall_clock_s = max(ended) - min(started)

    total_services = len(summaries)
    passed_services = sum(1 for item in summaries if item.get("exit_code", 1) == 0)

    total_tests = 0
    total_passed = 0
    total_failed = 0
    total_errors = 0
    total_skipped = 0
    for item in summaries:
        counts = item.get("counts", {})
        total_tests += int(counts.get("total", 0))
        total_passed += int(counts.get("passed", 0))
        total_failed += int(counts.get("failed", 0))
        total_errors += int(counts.get("errors", 0))
        total_skipped += int(counts.get("skipped", 0))

    lines.append("Integration Test Summary")
    lines.append(f"results_root: {results_root}")
    lines.append(
        f"services: {passed_services}/{total_services} passed, "
        f"{total_services - passed_services}/{total_services} failed"
    )
    lines.append(
        f"tests: {total_passed} passed, {total_failed} failed, "
        f"{total_errors} errors, {total_skipped} skipped, {total_tests} total"
    )
    lines.append(f"wall_clock: {_format_duration(wall_clock_s)}")
    lines.append("")

    for item in summaries:
        counts = item.get("counts", {})
        status = "PASSED" if item.get("exit_code", 1) == 0 else "FAILED"
        lines.append(
            f"{item.get('service_name')}: {status} in {_format_duration(float(item.get('duration_s', 0.0)))}"
        )
        lines.append(f"  target: {item.get('target')}")
        lines.append(
            "  counts: "
            f"{counts.get('passed', 0)} passed, "
            f"{counts.get('failed', 0)} failed, "
            f"{counts.get('errors', 0)} errors, "
            f"{counts.get('skipped', 0)} skipped, "
            f"{counts.get('total', 0)} total"
        )
        lines.append(f"  pytest log: {item.get('pytest_log')}")
        lines.append(f"  qemu log: {item.get('qemu_log')}")
        if item.get("idf_py_stdout_log"):
            lines.append(f"  idf stdout: {item.get('idf_py_stdout_log')}")
        if item.get("idf_py_stderr_log"):
            lines.append(f"  idf stderr: {item.get('idf_py_stderr_log')}")
        for test in item.get("tests", []):
            status_prefix = str(test.get("status", "unknown")).upper()
            entry = f"  {status_prefix}: {test.get('id')}"
            message = str(test.get("message", "")).strip()
            if message:
                entry += f" :: {message}"
            lines.append(entry)
        lines.append("")

    return lines, any_failed


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize Dockerized integration test results.")
    parser.add_argument(
        "--results-root",
        default="/tmp/coinbox-integration-results",
        help="Host directory that stores per-service integration test artifacts.",
    )
    args = parser.parse_args()

    results_root = Path(args.results_root).expanduser()
    summaries = _load_summaries(results_root)
    lines, any_failed = build_summary_lines(results_root, summaries)
    _print_colored_summary(results_root, summaries, any_failed)

    if results_root.exists():
        _write_summary_text(results_root / "summary.txt", lines)

    return 1 if any_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
