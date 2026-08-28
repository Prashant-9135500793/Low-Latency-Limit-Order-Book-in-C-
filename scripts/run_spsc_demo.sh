#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-./out/build/release}"
SHM_PATH="${SHM_PATH:-/tmp/lob_v2_demo_${$}.dat}"
JOURNAL_PATH="${JOURNAL_PATH:-/tmp/lob_v2_demo_${$}.journal}"
ORDERS="${ORDERS:-25000}"
LOG_DIR="${LOG_DIR:-/tmp/lob_v2_demo_${$}_logs}"
mkdir -p "$LOG_DIR"

cleanup() {
  jobs -pr | xargs -r kill 2>/dev/null || true
  rm -f "$SHM_PATH"
}
trap cleanup EXIT INT TERM

"$BUILD_DIR/matching_engine_main" \
  --path "$SHM_PATH" --journal "$JOURNAL_PATH" --idle-exit-ms 250 \
  >"$LOG_DIR/engine.log" 2>&1 &
engine_pid=$!

# Avoid opening the file in the brief create-before-ftruncate window.
for _ in $(seq 1 500); do
  current_size=0
  if [[ -f "$SHM_PATH" ]]; then
    current_size=$(stat -c%s "$SHM_PATH" 2>/dev/null || echo 0)
  fi
  if (( current_size > 1000000 )); then break; fi
  if ! kill -0 "$engine_pid" 2>/dev/null; then
    cat "$LOG_DIR/engine.log" >&2
    exit 1
  fi
  sleep 0.01
done

"$BUILD_DIR/market_viewer" --path "$SHM_PATH" --quiet \
  >"$LOG_DIR/viewer.log" 2>&1 &
viewer_pid=$!

"$BUILD_DIR/feed_handler" --path "$SHM_PATH" --orders "$ORDERS" \
  --advanced-orders --cancel-every 250 >"$LOG_DIR/feed.log" 2>&1

wait "$engine_pid"
wait "$viewer_pid"

cat "$LOG_DIR/feed.log"
tail -n 5 "$LOG_DIR/engine.log"
tail -n 5 "$LOG_DIR/viewer.log"
echo "Journal: $JOURNAL_PATH"
echo "Logs: $LOG_DIR"
