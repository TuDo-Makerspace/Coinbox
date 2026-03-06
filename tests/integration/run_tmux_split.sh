#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SESSION_NAME="${COINBOX_TMUX_SESSION:-coinbox-integration-tests}"
TEST_PORT="${COINBOX_TEST_PORT:-18080}"
TEST_BUILD_DIR="${COINBOX_TEST_BUILD_DIR:-build_qemu_integration}"
QEMU_LOG_PATH="${COINBOX_TEST_QEMU_LOG_PATH:-$REPO_ROOT/build_qemu_integration/qemu_test.log}"
TEST_TARGET="${COINBOX_INTEGRATION_TEST_TARGET:-tests/integration}"
IDF_EXPORT_SH="${IDF_EXPORT_SH:-}"
NO_ATTACH="${COINBOX_TMUX_NO_ATTACH:-0}"

if ! command -v tmux >/dev/null 2>&1; then
  echo "tmux is not installed."
  exit 1
fi

setup_cmd=""
if [[ -n "$IDF_EXPORT_SH" ]]; then
  if [[ ! -f "$IDF_EXPORT_SH" ]]; then
    echo "IDF_EXPORT_SH points to a missing file: $IDF_EXPORT_SH"
    exit 1
  fi
  setup_cmd="source \"$IDF_EXPORT_SH\" && "
elif ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py is not in PATH. Set IDF_EXPORT_SH or source ESP-IDF export.sh first."
  exit 1
fi

if tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
  tmux kill-session -t "$SESSION_NAME"
fi

tmux new-session -d -s "$SESSION_NAME" -n integration
# Right pane for QEMU output.
tmux split-window -h -t "$SESSION_NAME":0

# Keep output visible after command exit.
tmux set-option -t "$SESSION_NAME" remain-on-exit on

mkdir -p "$(dirname "$QEMU_LOG_PATH")"
: >"$QEMU_LOG_PATH"

qemu_tail_cmd="cd \"$REPO_ROOT\" && echo \"[tail] Following QEMU log: $QEMU_LOG_PATH\" && tail -n 0 -F \"$QEMU_LOG_PATH\" | python3 tests/integration/colorize_idf_log.py"
pytest_cmd="cd \"$REPO_ROOT\" && ${setup_cmd}COINBOX_TEST_PORT=${TEST_PORT} COINBOX_TEST_BUILD_DIR=${TEST_BUILD_DIR} COINBOX_TEST_QEMU_LOG_PATH=\"$QEMU_LOG_PATH\" python3 -m pytest -s -vv -rA \"$TEST_TARGET\"; rc=\$?; echo; echo \"[tmux] pytest exit code: \$rc\"; echo \"[tmux] Press Enter to close this tmux session.\"; read -r; tmux kill-session -t \"$SESSION_NAME\""

tmux send-keys -t "$SESSION_NAME":0.1 "$qemu_tail_cmd" C-m
tmux send-keys -t "$SESSION_NAME":0.0 "$pytest_cmd" C-m

tmux select-pane -t "$SESSION_NAME":0.0
if [[ "$NO_ATTACH" == "1" ]]; then
  echo "tmux session created: $SESSION_NAME"
  echo "Attach with: tmux attach -t $SESSION_NAME"
else
  tmux attach -t "$SESSION_NAME"
fi
