#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR="${1:-./out/build/release}"
exec "$BUILD_DIR/partitioned_matching_main" \
  --producers "${PRODUCERS:-4}" \
  --shards "${SHARDS:-4}" \
  --orders-per-producer "${ORDERS_PER_PRODUCER:-50000}" \
  --queue-capacity "${QUEUE_CAPACITY:-4096}" \
  --advanced-orders
