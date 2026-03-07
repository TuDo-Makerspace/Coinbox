#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
sys.path.insert(0, str(SCRIPT_DIR / "docker"))

from terminal_output import colors_enabled, status_style, strip_ansi, style


DEFAULT_SERVICES = [
    "integration-test-audio",
    "integration-test-auth",
    "integration-test-bootstrap",
    "integration-test-browser",
    "integration-test-gpio",
    "integration-test-sounds",
    "integration-test-system",
]


def _run_and_tee(
    cmd: list[str],
    cwd: Path,
    env: dict[str, str],
    output_path: Path,
    strip_for_log: bool = False,
) -> int:
    with output_path.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
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
            log_file.write(strip_ansi(line) if strip_for_log else line)
        return proc.wait()


def _prepare_results_root(requested_root: Path, color: bool) -> Path:
    results_root = requested_root
    if results_root.exists():
        try:
            shutil.rmtree(results_root)
        except PermissionError:
            suffix = datetime.now().strftime("%Y%m%d-%H%M%S")
            results_root = requested_root.with_name(f"{requested_root.name}-{suffix}")
            print(
                style(
                    "Existing results root could not be cleaned; using a fresh directory instead.",
                    "yellow",
                    enabled=color,
                )
            )

    results_root.mkdir(parents=True, exist_ok=True)
    return results_root


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Dockerized integration tests and print a summary.")
    parser.add_argument(
        "--results-root",
        default="/tmp/coinbox-integration-results",
        help="Host directory for pytest, QEMU, and summary artifacts.",
    )
    parser.add_argument(
        "--compose-file",
        default="tests/integration/docker-compose.yml",
        help="Compose file to use.",
    )
    parser.add_argument(
        "services",
        nargs="*",
        help="Optional subset of Compose services to run.",
    )
    args = parser.parse_args()

    color = colors_enabled()
    results_root = _prepare_results_root(Path(args.results_root).expanduser(), color)

    env = os.environ.copy()
    env["COINBOX_RESULTS_ROOT"] = str(results_root)
    env["COMPOSE_MENU"] = "false"
    if color:
        env["FORCE_COLOR"] = "1"
        env["CLICOLOR_FORCE"] = "1"
        env["PY_COLORS"] = "1"
        env.pop("NO_COLOR", None)
    else:
        env["NO_COLOR"] = "1"
        env["PY_COLORS"] = "0"
        env.pop("FORCE_COLOR", None)
        env.pop("CLICOLOR_FORCE", None)

    compose_cmd = [
        "docker",
        "compose",
        "--ansi",
        "auto" if color else "never",
        "-f",
        args.compose_file,
        "up",
        "--build",
    ]
    compose_cmd.extend(args.services or DEFAULT_SERVICES)

    compose_log_path = results_root / "compose.log"
    print(style("Running Docker integration tests", "cyan", "bold", enabled=color))
    print(f"results_root: {style(str(results_root), 'dim', enabled=color)}")
    compose_exit_code = _run_and_tee(
        compose_cmd,
        cwd=REPO_ROOT,
        env=env,
        output_path=compose_log_path,
        strip_for_log=True,
    )

    summary_cmd = [
        sys.executable,
        str(REPO_ROOT / "tests" / "integration" / "docker" / "summarize_docker_results.py"),
        "--results-root",
        str(results_root),
    ]
    summary_exit_code = subprocess.run(summary_cmd, cwd=REPO_ROOT, check=False).returncode

    final_status = "PASSED" if summary_exit_code == 0 and compose_exit_code == 0 else "FAILED"
    print(
        f"{style('Overall result', 'bold', enabled=color)}: "
        f"{status_style(final_status, enabled=color)}"
    )
    print(f"artifacts: {style(str(results_root), 'dim', enabled=color)}")

    return summary_exit_code if summary_exit_code != 0 else compose_exit_code


if __name__ == "__main__":
    raise SystemExit(main())
