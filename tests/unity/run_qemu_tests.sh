#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UNIT_TARGET="${1:-all}"

if ! command -v script >/dev/null 2>&1; then
  echo "script is not installed. Please install util-linux."
  exit 1
fi

run_unit_suite() {
  local project_path="$1"
  local build_dir="$2"

  script -qfc "cd \"$REPO_ROOT\" && timeout 45s idf.py -C \"$project_path\" -B \"$build_dir\" qemu --qemu-extra-args '-no-reboot'" /dev/null
}

case "$UNIT_TARGET" in
  all)
    run_unit_suite "$REPO_ROOT/tests/unity/files_props" "$REPO_ROOT/build_unity_files_props_test"
    run_unit_suite "$REPO_ROOT/tests/unity/recovery_code" "$REPO_ROOT/build_unity_recovery_code_test"
    ;;
  files)
    run_unit_suite "$REPO_ROOT/tests/unity/files_props" "$REPO_ROOT/build_unity_files_props_test"
    ;;
  recovery)
    run_unit_suite "$REPO_ROOT/tests/unity/recovery_code" "$REPO_ROOT/build_unity_recovery_code_test"
    ;;
  *)
    echo "Unknown unit test scope: $UNIT_TARGET"
    exit 1
    ;;
esac
