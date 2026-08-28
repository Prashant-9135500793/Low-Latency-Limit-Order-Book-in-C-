# Resume Bullets — Version 2

Use only bullets you can explain deeply in an interview. Throughput and latency numbers should be copied from a benchmark run on hardware you identify, not from another machine or from the historical report.

## Strong implementation bullets

- Designed and implemented a C++20 price-time-priority matching engine supporting limit/market orders, GTC/IOC/FOK/post-only policies, partial fills, integer-tick pricing, average O(1) cancellation, duplicate-ID protection, and fixed-size execution reports with explicit reject reasons.

- Built a pointer-free, memory-mapped three-process IPC protocol around a cache-line-aligned lock-free SPSC ring buffer using acquire/release atomics; added protocol version/size handshakes, lossless trade and execution-report channels, queue telemetry, and process heartbeats, validated with a real `fork()` cross-process test and a two-million-message FIFO stress test.

- Added deterministic crash recovery through a fixed-width little-endian append-only command journal with ordinal continuity, CRC32 corruption detection, buffered or per-command `fdatasync` durability, fail-stop write ordering, and offline replay that reconstructs book state and validates invariants.

- Corrected a subtle multi-symbol sequencing risk in a shared MPMC worker design by implementing symbol partitioning with one bounded FIFO and one owner thread per shard, preserving same-symbol command order while allowing independent shards to execute in parallel; instrumented end-to-end p50/p95/p99/p99.9 latency and queue high-water marks.

- Implemented configurable pre-trade risk controls (quantity, price collar, notional, active-order cap, kill switch), top-N depth snapshots, seeded randomized command-stream testing, sanitizer-ready CMake presets, GCC/Clang warnings-as-errors CI, Docker builds, and an optional Clang/libFuzzer target.

## Performance bullet template

- Processed **[measured throughput] orders/second** at **[measured p99]** end-to-end p99 latency across **[symbols/shards/producers]** on **[CPU, core count, compiler, build flags]**, while validating every book after each run and recording queue high-water pressure.

Fill every bracket from a fresh `benchmark_partitioned` or `benchmark_order_book` run. Keep the raw output with the project so the claim is reproducible.

## Interview framing

The strongest story is not “I made a toy exchange.” It is:

1. I preserved a clear correctness invariant: one writer per book and deterministic same-symbol sequencing.
2. I separated transport topology from matching logic: SPSC across processes and partitioned fan-in across threads solve different problems.
3. I made command outcomes, risk policy, persistence, recovery, telemetry, and failure behavior explicit.
4. I tested corruption, truncation, races, cross-process visibility, randomized command streams, and tail latency instead of reporting only a happy-path demo.
5. I can explain what remains non-production and why adding kernel bypass before measuring the actual bottleneck would be premature.
