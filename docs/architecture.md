# Architecture

> **Version 2 note:** This chapter documents the original three-process SPSC path. The current `SharedRegion` also carries execution reports, protocol-size validation, counters, and heartbeats. See [`v2_upgrade.md`](v2_upgrade.md) and [`persistence.md`](persistence.md).

## Overview

Three independent OS processes cooperate over a single memory-mapped
shared-memory file to simulate a (toy) trading pipeline:

```
 feed_handler                matching_engine_main                  market_viewer
 ------------                --------------------                  -------------
 generates OrderMsg,         validates and optionally journals     drains Trade and
 including cancels,          each command, runs MatchingEngine,    ExecutionReport
 and pushes into       --->  publishes trades + reports,     ---> messages; polls BBO,
 order_queue                 counters, BBO, and heartbeat          counters, queue depth,
 (SPSC producer)             (order consumer; two producers)       and heartbeat age

        \______________________________  ______________________________/
                                       \/
                     one mmap()-ed file: /tmp/lob_shared_memory.dat
              SharedRegion { header, order_queue, trade_queue, report_queue }
```

`matching_engine_main` is the only process that **creates** the shared
file (it calls `ftruncate` to size it and placement-constructs the
`SharedRegion` layout in it). `feed_handler` and `market_viewer` only
**open** the existing file and `mmap()` it — they never assume
anything about where matching_engine_main happened to load, run, or
map its own memory; the only shared contract is the byte layout of
`SharedRegion`, which both sides get by mapping the same C++ struct
type over the same file.

## Process roles

| Process | Role | Reads | Writes |
|---|---|---|---|
| `matching_engine_main` | creator, optional recovery/journal owner, matching logic | `order_queue` | `trade_queue`, `report_queue`, BBO/counters/heartbeat/state in `header` |
| `feed_handler` | new-order/cancel producer | protocol metadata, READY state, engine heartbeat | `order_queue`, feed heartbeat |
| `market_viewer` | trade/report/telemetry consumer | `trade_queue`, `report_queue`, BBO/counters/heartbeats | viewer heartbeat |

## Data flow

1. `matching_engine_main` starts, creates+sizes the backing file,
   placement-constructs `SharedRegion`, and finally publishes
   `header.state = READY` with a `release` store.
2. `feed_handler` polls until it observes `state == READY` (`acquire`
   load), then generates deterministic synthetic `Order`s and
   `try_push`es `OrderMsg{NEW_ORDER, order}` into `order_queue`.
3. `matching_engine_main`'s main loop `try_pop`s from `order_queue`,
   optionally appends the command to the CRC-protected journal, and runs
   it through `MatchingEngine::processOrder` or `MatchingEngine::cancel`.
   It losslessly publishes every resulting `Trade` to `trade_queue` and
   one `ExecutionReport` to `report_queue`, then updates BBO, counters,
   the command sequence, and its heartbeat in the header.
4. `market_viewer` polls until READY, drains both outbound queues, and
   periodically prints BBO/spread, queue occupancy, accepted/rejected/
   canceled/trade counts, and heartbeat age. It exits only after the
   engine marks the region finished and both outbound queues are empty.

## In-process vs cross-process reuse

The same `SPSCQueue<T, Capacity>` template is used both for the
in-process unit tests/benchmarks (`benchmarks/benchmark_spsc.cpp`) and
for the cross-process shared-memory queues inside `SharedRegion`. This
works because the queue's entire state (indices + backing array) is
stored **by value**, with no pointers into its own address space — see
`docs/ipc.md` for why that specific property is what makes a data
structure safe to place in shared memory.

## Executables

- `matching_engine_main` — start this first.
- `feed_handler [--orders N] [--seed N] [--path P]` — start after the
  engine; can generate GTC/IOC/FOK/market/post-only orders and cancels,
  then exits after all commands are queued.
- `market_viewer [--path P] [--quiet]` — start any time after the engine;
  runs until the engine reports it is done and both outbound queues are
  drained.
- `cleanup_shared_memory [--path P]` — removes the backing file.

See the top-level `README.md` for exact build/run commands.
