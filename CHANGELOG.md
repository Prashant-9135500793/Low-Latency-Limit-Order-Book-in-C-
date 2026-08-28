# Changelog

## 2.0.0

### Added

- Market orders, IOC, FOK, and post-only policy.
- Fixed-size execution reports and detailed rejection reasons.
- Pre-trade quantity, price, notional, active-order, and kill-switch controls.
- Top-N aggregated book-depth snapshots and stronger invariant checks.
- CRC32-protected append-only command journal, replay, recovery, and offline inspector.
- Shared-memory protocol v2 with execution-report queue, layout-size validation, counters, and heartbeats.
- Deterministic symbol-partitioned owner-thread matching architecture.
- End-to-end logarithmic latency histogram and queue high-water telemetry.
- Partitioned demo and benchmark.
- Randomized 75,000-command stress test and optional libFuzzer target.
- CMake presets, warning controls, LTO option, CI, Docker, and demo scripts.

### Changed

- Accepted client order IDs are unique for the engine lifetime.
- `OrderBook::addOrder` validates malformed/duplicate active orders and returns success/failure.
- Cancel execution reports include the removed resting quantity.
- Legacy shared-worker MPMC path is explicitly documented as a comparison architecture because locking alone does not preserve same-symbol arrival order.
- Real-data generation keeps flat daily ranges exactly bounded instead of widening them by one tick.

## 1.0.0

- Original price-time-priority order book and matching engine.
- Memory-mapped lock-free SPSC IPC across feed, engine, and viewer processes.
- Mutex/condition-variable MPMC queue and shard-locked multi-symbol extension.
- Unit/integration tests, sanitizers, benchmarks, and historical AAPL-derived input mode.
