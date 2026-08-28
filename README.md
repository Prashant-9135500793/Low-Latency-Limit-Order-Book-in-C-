# Low-Latency Limit Order Book — Advanced C++20 Edition

A compact, testable trading-systems core written in C++20. The project now combines:

- a price-time-priority limit order book and matching engine;
- GTC, IOC, FOK, market, and post-only order semantics;
- explicit execution reports and pre-trade risk controls;
- an append-only CRC-protected command journal with deterministic recovery;
- memory-mapped, lock-free SPSC IPC across three Linux processes;
- a deterministic multi-instrument architecture with one FIFO queue and one owner thread per shard;
- end-to-end latency histograms, queue-pressure metrics, randomized stress testing, sanitizers, CI, Docker, and an optional libFuzzer target.

This is a serious educational/reference implementation, not a production exchange. It deliberately keeps authentication, exchange wire protocols, replication, kernel-bypass networking, and real-money connectivity out of scope so the matching, concurrency, persistence, and correctness mechanisms remain inspectable.

## What changed in version 2

| Area | Upgrade |
|---|---|
| Order semantics | `LIMIT` and `MARKET`; `GTC`, `IOC`, `FOK`; `POST_ONLY` |
| Client feedback | Fixed-size `ExecutionReport` for accepted, resting, filled, expired, canceled, and rejected commands |
| Risk | Maximum quantity, price collar, maximum notional, active-order cap, and kill switch |
| Persistence | Fixed-width little-endian journal records, ordinal continuity, CRC32 validation, buffered or `fdatasync` durability, replay/recovery |
| Multi-symbol concurrency | Recommended partitioned owner-thread engine that preserves same-symbol FIFO sequencing |
| Market data | Top-of-book plus aggregated top-N depth snapshots |
| Operations | Protocol version/region-size handshake, process heartbeats, queue-depth and command counters |
| Performance | Allocation-free logarithmic latency histogram with p50/p95/p99/p99.9 estimates and queue high-water marks |
| Quality | Nine CTest executables, randomized command-stream tests, ASan/UBSan, TSan configuration, CI, CMake presets, Docker, optional fuzzing |

## Architecture

### 1. Three-process, single-instrument SPSC IPC

```text
 feed_handler                  matching_engine_main                market_viewer
 ────────────                  ────────────────────                ─────────────
 generates OrderMsg  ───────▶  validates + journals commands      drains Trades
                                 runs MatchingEngine       ───────▶ drains ExecutionReports
 SPSC producer                  SPSC consumer/producer              SPSC consumer

              one MAP_SHARED file containing a pointer-free SharedRegion
        header + order_queue + trade_queue + report_queue
```

The producer and consumer counters are separated onto cache lines. Payload publication uses release stores and acquire loads. Once the mapping exists, each message transfer is ordinary shared-memory loads/stores with no per-message syscall or serialization step.

### 2. Recommended multi-instrument partitioned engine

```text
 many producer threads
          │
          ├── hash(symbol) ──▶ shard 0 bounded FIFO ──▶ owner thread 0 ──▶ books
          ├── hash(symbol) ──▶ shard 1 bounded FIFO ──▶ owner thread 1 ──▶ books
          └── hash(symbol) ──▶ shard N bounded FIFO ──▶ owner thread N ──▶ books
```

Every book is mutated by exactly one shard owner. Concurrent producers are linearized by the selected shard queue, so commands for the same symbol cannot be processed in a different order merely because two workers acquired a mutex in the opposite order. Different shards can execute in parallel on different cores.

### 3. Legacy shared-worker MPMC comparison

`mpmc_matching_main` is retained to demonstrate a bounded mutex/condition-variable MPMC queue feeding a shard-locked engine. It is data-race safe, but a shared worker pool can pop two same-symbol commands in FIFO order and acquire the shard lock in reverse order. Use it as a comparison architecture; use `partitioned_matching_main` when deterministic same-symbol sequencing matters.

More detail:

- [`docs/v2_upgrade.md`](docs/v2_upgrade.md)
- [`docs/order_semantics.md`](docs/order_semantics.md)
- [`docs/partitioned_engine.md`](docs/partitioned_engine.md)
- [`docs/persistence.md`](docs/persistence.md)
- Original deep dives remain under [`docs/`](docs/).

## Matching semantics

The engine executes at the resting order's price and preserves FIFO within each price level.

- **GTC limit:** execute immediately, then rest any remainder.
- **IOC limit/market:** execute immediately and expire any remainder; never rest.
- **FOK:** preflight all immediately executable liquidity and either fill the entire quantity or reject without mutating the book.
- **Post-only:** accept only a non-crossing GTC limit order; reject rather than take liquidity.
- **Market:** must be IOC or FOK because an unpriced order cannot rest.

Prices are integer ticks, never floating point. Active cancellation is an average O(1) hash lookup plus list erase. A client order ID is consumed for the lifetime of an engine after a command is accepted, even when an IOC remainder expires, which keeps audit/replay semantics unambiguous.

## Build

Linux is required for `mmap`, `fork`, POSIX file descriptors, and `fdatasync`. A C++20 compiler and CMake 3.20 or newer are required.

Using presets:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Other presets:

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
cmake --preset tsan  && cmake --build --preset tsan
```

Classic out-of-source build remains supported:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful CMake options:

```text
LOB_ENABLE_ASAN_UBSAN=ON
LOB_ENABLE_TSAN=ON
LOB_ENABLE_LTO=ON
LOB_WARNINGS_AS_ERRORS=ON
LOB_BUILD_BENCHMARKS=OFF
LOB_BUILD_FUZZERS=ON        # Clang/libFuzzer only
```

## Run the advanced three-process demo

Start the engine first, then the viewer, then the feed:

```bash
# Terminal 1
./out/build/release/matching_engine_main \
  --journal /tmp/lob.commands.journal \
  --max-order-qty 1000 \
  --max-notional 20000000

# Terminal 2
./out/build/release/market_viewer --quiet

# Terminal 3
./out/build/release/feed_handler \
  --orders 100000 \
  --advanced-orders \
  --cancel-every 250 \
  --seed 42
```

The viewer drains both trades and execution reports and prints protocol telemetry, queue depths, BBO, command counters, and engine-heartbeat age. Keep the viewer running for high-volume tests: both outbound queues are intentionally lossless and bounded, so the engine applies backpressure instead of silently dropping events.

A convenience script starts all three components with isolated temporary paths:

```bash
./scripts/run_spsc_demo.sh ./out/build/release
```

### Recovery and offline journal inspection

Recover the live engine from a journal, then continue appending:

```bash
./out/build/release/matching_engine_main \
  --journal /tmp/lob.commands.journal \
  --recover
```

Validate and inspect a journal without starting shared memory:

```bash
./out/build/release/journal_replay_main \
  --journal /tmp/lob.commands.journal \
  --depth 10
```

Replay must use the same risk-limit configuration that was active when the journal was written. The journal records the command stream; risk policy is deployment configuration.

`--sync-journal` calls `fdatasync` after every command for stronger crash durability at the cost of latency. Buffered mode syncs once during orderly shutdown.

## Run the recommended partitioned engine

```bash
./out/build/release/partitioned_matching_main \
  --producers 8 \
  --shards 8 \
  --orders-per-producer 200000 \
  --queue-capacity 8192 \
  --advanced-orders
```

The program reports:

- submitted/processed/accepted/rejected commands and generated trades;
- throughput;
- approximate end-to-end p50, p95, p99, and p99.9 latency;
- maximum per-shard queue occupancy;
- per-symbol BBO, depth, active orders, sequence, recent trades, and invariant status.

Convenience wrapper:

```bash
./scripts/run_partitioned_demo.sh ./out/build/release
```

## Real historical data mode

The original AAPL daily-data mode remains available:

```bash
./out/build/release/feed_handler \
  --real-data \
  --data-file ./data/AAPL.csv \
  --orders-per-day 40 \
  --seed 7
```

Dates, OHLC values, and volume come from the included historical dataset. Individual orders are necessarily synthesized; each generated price remains inside that day's recorded low/high interval, and sizes are scaled by recorded daily volume. The generator exits on missing/malformed data rather than silently substituting synthetic data.

## Testing and validation

```bash
ctest --test-dir out/build/release --output-on-failure
```

The test suite covers:

- order-book priority, cancellation, partial reduction, depth snapshots, and structural invariants;
- exact, partial, multi-level, FIFO, market, IOC, FOK, post-only, duplicate-ID, risk, kill-switch, and execution-report behavior;
- two-million-message SPSC FIFO correctness;
- real `fork()` + `mmap()` cross-process IPC;
- MPMC no-loss/no-duplication and shutdown behavior;
- partitioned-engine multi-producer correctness and per-symbol invariants;
- journal replay, append, CRC corruption, and truncation detection;
- a seeded 75,000-command randomized stress stream plus randomized risk-limit checks.

CI builds GCC and Clang in Debug and Release with warnings-as-errors, runs ASan/UBSan, and runs the in-process concurrency tests under TSan.

Optional fuzzing with Clang:

```bash
cmake -S . -B out/build/fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLOB_BUILD_FUZZERS=ON
cmake --build out/build/fuzz --target fuzz_matching_engine
./out/build/fuzz/fuzz_matching_engine ./fuzz-corpus
```

## Benchmarks

```bash
./out/build/release/benchmark_order_book
./out/build/release/benchmark_spsc
./out/build/release/benchmark_ipc
./out/build/release/benchmark_partitioned --orders 1000000
```

Benchmark output is environment-specific. Multi-thread scaling should be measured on dedicated, non-throttled multi-core hardware with CPU topology, compiler, build flags, and affinity recorded. The partitioned benchmark validates every book after each run and labels invalid runs rather than reporting throughput alone.

## Docker

```bash
docker build -t low-latency-lob-v2 .
docker run --rm low-latency-lob-v2
```

The image builds the project, runs CTest in the build stage, and defaults to the advanced partitioned demo.

## Source layout

```text
include/
  order.hpp, execution_report.hpp, risk_manager.hpp
  order_book.hpp, matching_engine.hpp
  spsc_queue.hpp, mpmc_queue.hpp, partitioned_engine.hpp
  shared_types.hpp, shared_memory.hpp
  journal.hpp, latency_histogram.hpp
src/
apps/
  feed_handler, matching_engine_main, market_viewer
  partitioned_matching_main, journal_replay_main
  mpmc_matching_main (legacy comparison)
tests/
benchmarks/
fuzz/
docs/
.github/workflows/ci.yml
```

## Honest remaining scope

This version closes several important prototype gaps, but a real venue would still require authenticated binary order-entry and market-data protocols, session sequencing and resend, self-trade prevention, richer risk and position accounting, durable replicated consensus/failover, clock synchronization, production observability, security hardening, load shedding policy, NUMA-aware placement, and independently audited correctness. Kernel-bypass networking should only be added after the end-to-end design and bottlenecks are measured on representative hardware.
