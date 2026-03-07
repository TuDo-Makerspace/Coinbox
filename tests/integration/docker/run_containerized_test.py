from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from terminal_output import color_mode, colors_enabled, status_style, strip_ansi, style


def _required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"Missing required environment variable: {name}")
    return value


def _ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def _latest_log_file(log_dir: Path, prefix: str) -> Path | None:
    candidates = sorted(log_dir.glob(f"{prefix}*"), key=lambda path: path.stat().st_mtime)
    return candidates[-1] if candidates else None


def _copy_if_exists(src: Path | None, dest: Path) -> str | None:
    if src is None or not src.is_file():
        return None
    shutil.copy2(src, dest)
    return str(dest)


def _first_message_line(node: ET.Element | None) -> str:
    if node is None:
        return ""

    parts = []
    message = (node.get("message") or "").strip()
    if message:
        parts.append(message)
    text = (node.text or "").strip()
    if text:
        parts.append(text)

    for line in "\n".join(parts).splitlines():
        stripped = line.strip()
        if stripped:
            return stripped
    return ""


def _parse_junit_report(path: Path) -> tuple[dict[str, int], list[dict[str, object]]]:
    counts = {"total": 0, "passed": 0, "failed": 0, "errors": 0, "skipped": 0}
    testcases: list[dict[str, object]] = []

    if not path.is_file():
        return counts, testcases

    root = ET.parse(path).getroot()
    for case in root.findall(".//testcase"):
        counts["total"] += 1

        failure = case.find("failure")
        error = case.find("error")
        skipped = case.find("skipped")

        if failure is not None:
            status = "failed"
            counts["failed"] += 1
            message = _first_message_line(failure)
        elif error is not None:
            status = "error"
            counts["errors"] += 1
            message = _first_message_line(error)
        elif skipped is not None:
            status = "skipped"
            counts["skipped"] += 1
            message = _first_message_line(skipped)
        else:
            status = "passed"
            counts["passed"] += 1
            message = ""

        classname = case.get("classname", "").strip()
        name = case.get("name", "").strip()
        test_id = f"{classname}::{name}" if classname else name
        try:
            duration_s = float(case.get("time", "0") or 0.0)
        except ValueError:
            duration_s = 0.0

        testcases.append(
            {
                "id": test_id,
                "status": status,
                "duration_s": duration_s,
                "message": message,
            }
        )

    return counts, testcases


def _write_summary(
    path: Path,
    service_name: str,
    target: str,
    artifact_dir: Path,
    qemu_log_path: str,
    junit_path: Path,
    pytest_log_path: Path,
    build_stdout_copy: str | None,
    build_stderr_copy: str | None,
    start_time_s: float,
    end_time_s: float,
    exit_code: int,
    counts: dict[str, int],
    tests: list[dict[str, object]],
):
    status = "passed" if exit_code == 0 else "failed"
    payload = {
        "service_name": service_name,
        "target": target,
        "status": status,
        "exit_code": exit_code,
        "started_at_epoch_s": start_time_s,
        "ended_at_epoch_s": end_time_s,
        "duration_s": end_time_s - start_time_s,
        "artifact_dir": str(artifact_dir),
        "pytest_log": str(pytest_log_path),
        "junit_xml": str(junit_path),
        "qemu_log": qemu_log_path,
        "idf_py_stdout_log": build_stdout_copy,
        "idf_py_stderr_log": build_stderr_copy,
        "counts": counts,
        "tests": tests,
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def _print_container_summary(
    service_name: str,
    target: str,
    duration_s: float,
    counts: dict[str, int],
    exit_code: int,
    pytest_log_path: Path,
    qemu_log_path: str,
):
    color = colors_enabled()

    print("", flush=True)
    print(style(f"[summary] {service_name}", "cyan", "bold", enabled=color), flush=True)
    print(f"  target: {style(target, 'blue', enabled=color)}", flush=True)
    print(
        f"  status: {status_style('PASSED' if exit_code == 0 else 'FAILED', enabled=color)}",
        flush=True,
    )
    print(
        "  counts: "
        f"{style(str(counts['passed']), 'green', enabled=color)} passed, "
        f"{style(str(counts['failed']), 'red', enabled=color)} failed, "
        f"{style(str(counts['errors']), 'red', enabled=color)} errors, "
        f"{style(str(counts['skipped']), 'yellow', enabled=color)} skipped, "
        f"{style(str(counts['total']), 'bold', enabled=color)} total",
        flush=True,
    )
    print(f"  duration: {style(f'{duration_s:.1f}s', 'magenta', enabled=color)}", flush=True)
    print(f"  pytest log: {style(str(pytest_log_path), 'dim', enabled=color)}", flush=True)
    print(f"  qemu log: {style(qemu_log_path, 'dim', enabled=color)}", flush=True)


def main() -> int:
    service_name = _required_env("COINBOX_SERVICE_NAME")
    target = _required_env("TEST_TARGET")
    artifact_dir = _ensure_dir(Path(_required_env("COINBOX_TEST_ARTIFACT_DIR")))
    qemu_log_path = _required_env("COINBOX_TEST_QEMU_LOG_PATH")
    build_dir = Path(_required_env("COINBOX_TEST_BUILD_DIR"))

    pytest_log_path = artifact_dir / "pytest.log"
    junit_path = artifact_dir / "junit.xml"
    summary_json_path = artifact_dir / "summary.json"

    start_time_s = time.time()
    env = os.environ.copy()
    cmd = [
        sys.executable,
        "-m",
        "pytest",
        "-s",
        "-vv",
        "-rA",
        f"--color={color_mode()}",
        "--junitxml",
        str(junit_path),
        target,
    ]

    with pytest_log_path.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(
            cmd,
            cwd="/work",
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            bufsize=1,
        )

        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            log_file.write(strip_ansi(line))
        exit_code = proc.wait()

    end_time_s = time.time()

    build_log_dir = build_dir / "log"
    stdout_copy = None
    stderr_copy = None
    if build_log_dir.is_dir():
        stdout_copy = _copy_if_exists(
            _latest_log_file(build_log_dir, "idf_py_stdout_output_"),
            artifact_dir / "idf_py_stdout.log",
        )
        stderr_copy = _copy_if_exists(
            _latest_log_file(build_log_dir, "idf_py_stderr_output_"),
            artifact_dir / "idf_py_stderr.log",
        )

    counts, tests = _parse_junit_report(junit_path)
    _write_summary(
        path=summary_json_path,
        service_name=service_name,
        target=target,
        artifact_dir=artifact_dir,
        qemu_log_path=qemu_log_path,
        junit_path=junit_path,
        pytest_log_path=pytest_log_path,
        build_stdout_copy=stdout_copy,
        build_stderr_copy=stderr_copy,
        start_time_s=start_time_s,
        end_time_s=end_time_s,
        exit_code=exit_code,
        counts=counts,
        tests=tests,
    )
    _print_container_summary(
        service_name=service_name,
        target=target,
        duration_s=end_time_s - start_time_s,
        counts=counts,
        exit_code=exit_code,
        pytest_log_path=pytest_log_path,
        qemu_log_path=qemu_log_path,
    )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
