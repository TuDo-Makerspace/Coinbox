#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import shutil
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DOCKER_CLEAN_IMAGE = "debian:latest"


def _is_build_dir(path: Path) -> bool:
    if not path.is_dir():
        return False
    return path.name == "build" or path.name.startswith("build_")


def main() -> int:
    removed = []
    needs_docker = []

    for path in sorted(REPO_ROOT.iterdir()):
        if not _is_build_dir(path):
            continue
        try:
            shutil.rmtree(path)
        except PermissionError:
            needs_docker.append(path.name)
        else:
            removed.append(path.name)

    if needs_docker:
        cmd = [
            "docker",
            "run",
            "--rm",
            "-v",
            f"{REPO_ROOT}:/work",
            "-w",
            "/work",
            DOCKER_CLEAN_IMAGE,
            "sh",
            "-lc",
            'rm -rf -- "$@"',
            "sh",
            *needs_docker,
        ]
        try:
            result = subprocess.run(cmd, check=False)
        except FileNotFoundError:
            print("Failed to remove root-owned build directories: docker not found.")
            print("Still present:")
            for name in needs_docker:
                print(f"- {name}")
            return 1

        if result.returncode != 0:
            print("Failed to remove root-owned build directories with docker.")
            print("Command:")
            print(" ".join(cmd))
            print("Still present:")
            for name in needs_docker:
                print(f"- {name}")
            return result.returncode or 1

        removed.extend(needs_docker)

    if removed:
        print("Removed build directories:")
        for name in removed:
            print(f"- {name}")
    else:
        print("No build directories found.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
