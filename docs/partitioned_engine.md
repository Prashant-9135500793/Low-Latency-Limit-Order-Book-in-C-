# Deterministic Partitioned Matching Engine

## Goal

Scale across instruments while preserving the fundamental invariant that one order book has one writer and that commands for one symbol execute in a well-defined FIFO order.

## Topology

```text
Producer 0 ─┐
Producer 1 ─┼─▶ hash(Symbol) ─▶ bounded queue for shard k ─▶ owner k ─▶ engines[Symbol]
Producer N ─┘
```

Each shard owns:

- one bounded queue;
- one dedicated worker/owner thread;
- an `unordered_map<Symbol, MatchingEngine>`;
- a small mutex used only to make external snapshots safe while the owner mutates state.

The queue is implemented with a mutex and condition variables because it has many producers and one consumer. Correct bounded backpressure and shutdown are prioritized over presenting an unverified lock-free algorithm.

## Ordering guarantee

Concurrent producers do not have a pre-existing total order. The successful insertion into a shard queue is the linearization point. From that point onward, the shard owner processes commands in exact FIFO order. All commands for one symbol always route to the same shard.

This is stronger than a shared worker pool plus shard mutex. A mutex guarantees mutual exclusion, not acquisition order.

## Parallelism

Different shards have different queues and owner threads, so they can execute on separate cores. Symbols that hash to the same shard serialize; increasing the shard count can reduce unrelated-symbol contention, but too many shards add threads and scheduling overhead. The shard count must be a power of two because routing uses a bit mask.

## Backpressure and shutdown

- `enqueue` blocks when a shard queue is full and returns false only during shutdown.
- `tryEnqueue` never blocks and returns false when full or shutting down.
- `submit` and `cancel` return futures containing an execution report and generated trades.
- `shutdown` stops acceptance, closes all queues, drains queued work, and joins every owner.
- `waitUntilIdle` waits until all accepted queue entries have completed.

The constructor is exception-safe: if creation of one owner thread fails, already-started owners are stopped and joined before the exception is rethrown.

## Metrics

`PartitionedEngineStats` reports:

- submitted, processed, accepted, rejected, trades, and pending commands;
- maximum observed queue occupancy across shards;
- end-to-end latency from pre-enqueue timestamp to completed processing.

The latency histogram has 64 logarithmic power-of-two nanosecond buckets. Recording is allocation-free and uses relaxed atomics. Percentiles are approximate bucket upper bounds, explicitly labeled as such.

## Snapshot consistency

`snapshot(symbol)` takes the selected shard's state mutex and returns BBO, order count, side depths, and engine sequence. It is a point-in-time copy for that shard. It is not an atomic snapshot across all symbols; obtaining a global simultaneous view would require a coordination protocol that would interfere with independent shard progress.
