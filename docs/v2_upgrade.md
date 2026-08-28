# Version 2 Upgrade Review

## Assessment of the original project

The original code was already stronger than a typical portfolio CRUD project. It demonstrated price-time priority, exact integer-tick prices, average O(1) cancellation, a pointer-free `mmap` layout, acquire/release SPSC synchronization, false-sharing avoidance, a multi-threaded MPMC extension, sanitizer runs, and measured benchmarks.

The main limitation was not that the core idea was weak; it was that the implementation stopped at the boundary between a focused systems prototype and a production-shaped trading core. The upgrade therefore keeps the original depth and adds the missing lifecycle around an order: validation, policy, client acknowledgement, persistence, replay, deterministic multi-symbol sequencing, operational telemetry, and automated quality gates.

## Implemented upgrade map

| Original gap | Version 2 implementation |
|---|---|
| Limit orders only | Market orders plus GTC, IOC, FOK, and post-only |
| Trade count as the primary result | Explicit fixed-size `ExecutionReport` with status and reject reason |
| No pre-trade checks | Quantity, price collar, notional, active-order cap, kill switch |
| No durable event history | Append-only little-endian journal, ordinal checks, CRC32, replay, optional per-record `fdatasync` |
| Shared MPMC workers can reorder same-symbol lock acquisition | One bounded FIFO and one owner thread per symbol shard |
| Aggregate throughput only | Live logarithmic latency histogram and queue high-water mark |
| BBO only | Top-N aggregated level snapshots |
| Weak protocol diagnostics | Magic/version/region-size validation, counters, queue depth, process heartbeats |
| Manual quality workflow | CMake presets, GCC/Clang CI, warnings-as-errors, ASan/UBSan, TSan, Docker |
| Scenario tests only | Seeded randomized command stream and optional libFuzzer target |

## Key architectural correction: sequencing is not the same as locking

The original lock-based `ShardedMatchingEngine` prevents two workers from mutating one shard at the same instant. That solves data races. It does not prove arrival-order preservation when multiple workers consume one shared queue:

1. worker A pops AAPL command 1;
2. worker B pops AAPL command 2;
3. worker B reaches the shard mutex first;
4. command 2 executes before command 1.

The book remains structurally valid, but price-time behavior can change. `PartitionedMatchingEngine` routes both commands into the same shard FIFO and gives that FIFO one consumer/owner. Queue insertion is the linearization point for concurrent producers; the owner processes the resulting order strictly. Parallelism comes from different shards, not simultaneous mutation of one book.

The legacy MPMC path is intentionally retained because the comparison itself is valuable: mutual exclusion, logical sequencing, and parallel scaling are separate design concerns.

## Correctness policy

The upgraded engine follows these rules:

- Every command, including rejects and cancels, receives a monotonically increasing engine sequence.
- Accepted client order IDs are never reusable during the engine lifetime.
- FOK preflights liquidity before mutating the book.
- IOC and market remainders never rest.
- Post-only commands reject rather than remove liquidity.
- The execution price is the resting order's price.
- Journal append happens before command application in the live SPSC engine; a journal failure is fail-stop rather than silently applying an unrecorded command.
- Recovery passes journaled commands through the same matching code used live.
- Every partition has a single writer, and every test/benchmark validates book invariants.

## Performance policy

The project avoids claiming universal low-latency numbers. Results depend on CPU model, physical core count, scheduler noise, frequency scaling, NUMA layout, compiler, and build flags. The supplied benchmark programs:

- pre-generate work where appropriate;
- use a monotonic clock;
- report tail latency as well as throughput;
- expose queue high-water marks;
- validate correctness after timed runs;
- make the histogram's power-of-two approximation explicit.

For a credible performance report, rerun on dedicated multi-core Linux hardware, record topology and affinity, disable or document frequency scaling, perform repeated trials, and publish the raw output rather than copying numbers from another machine.

## Remaining production work

Version 2 is intentionally still not a venue. The next technically meaningful layers would be:

1. authenticated binary TCP/UDP sessions with sequence numbers, replay/resend, and malformed-message isolation;
2. participant/account identity, self-trade prevention, position limits, and richer credit/risk accounting;
3. replicated durable state, checkpoints, deterministic failover, and recovery-time objectives;
4. independent market-data publication with snapshots plus incremental updates;
5. CPU affinity, NUMA-aware allocation, huge-page evaluation, and topology-aware benchmark automation;
6. security review, resource quotas, load-shedding policy, structured metrics, and on-call diagnostics.

Adding those without weakening the current invariants is more valuable than adding many loosely connected features.
