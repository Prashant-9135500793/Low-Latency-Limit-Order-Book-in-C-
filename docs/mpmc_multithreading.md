# Multi-producer / multi-consumer multithreading (in-process)

> **Version 2 note:** This chapter describes the legacy shared-worker MPMC comparison. It is data-race safe, but a shared worker pool plus shard mutex does not guarantee same-symbol processing order. The recommended owner-thread design is documented in [`partitioned_engine.md`](partitioned_engine.md).

The original pipeline (`feed_handler` -> `matching_engine_main` ->
`market_viewer`) is deliberately **single-producer/single-consumer**:
one process writes orders, one process reads them, over the
lock-free `SPSCQueue` in shared memory. That's the right design for
"one instrument, one feed source, one matching thread" — see
`docs/architecture.md`.

This adds the other shape: **several producer threads, several
consumer threads, several instruments, all in one process**, which is
what `mpmc_matching_main` demonstrates.

```
 producer thread 0  \                                      / worker thread 0 -\
 producer thread 1   >---> MPMCQueue<SymbolOrder> --------->  worker thread 1   >--> ShardedMatchingEngine
 producer thread ..  /        (mutex + condvar,             \ worker thread ..-/       (per-symbol books,
                                bounded, blocking)                                      one mutex per shard)
```

## New pieces

| File | What it is | Why it's not just `spsc_queue.hpp` / `order_book.hpp` again |
|---|---|---|
| `include/mpmc_queue.hpp` | Bounded, blocking **multi**-producer/**multi**-consumer queue (`std::mutex` + two `std::condition_variable`s, backed by `std::deque`) | `SPSCQueue` only stays lock-free *because* it assumes exactly one writer and one reader thread. Add a second producer and two threads can race on `head_` — that's a real data race, not a style choice. A correct lock-free MPMC ring buffer (Vyukov-style, per-slot sequence numbers) is a much bigger undertaking; the mutex version is correct, easy to audit, and not the bottleneck here (the matching engine's book mutations are). |
| `include/symbol.hpp` | Fixed-width `Symbol` + `SymbolOrder` (order tagged with instrument) | The original `Order` has no symbol field because the original programs only ever handle one instrument. Multiple instruments need a way to say *which* book an order belongs to so it can be routed. |
| `include/sharded_engine.hpp` | `ShardedMatchingEngine`: symbol -> shard (hash) -> `{mutex, unordered_map<Symbol, MatchingEngine>}` | The project's own invariant (see `matching_engine.hpp`, `docs/interview_questions.md`) is that one order book must have **one writer at a time**. Sharding by symbol keeps that invariant (each book still has a single owner at any instant, guarded by its shard's mutex) while letting workers touch *different* symbols fully in parallel — real multi-core throughput without ever locking two threads onto the same book. |
| `include/trade_log.hpp` | Thread-safe, `std::deque`-backed "last N trades per symbol" ring | Genuinely different access pattern from the book: push at the back on every trade, drop from the front once over the cap, and support O(1) random access ("show me trade #k") for a viewer/replay tool. `std::deque` gives O(1) at both ends *and* O(1) indexing — `std::vector` needs an O(n) shift on `pop_front`, `std::list` (used deliberately elsewhere for the book's cancel-anywhere FIFOs) has no O(1) random access. Different job, different container — used *alongside* `std::map`/`std::list`/`std::unordered_map`, not instead of them (the book itself is untouched and still has to stay a `std::map`/`std::list`/`std::unordered_map` structure, since that's what gives cancel-by-id its O(log P) guarantee). |
| `apps/mpmc_matching_main.cpp` | Demo/benchmark: N producer threads + M consumer threads + a handful of instruments | `--producers`, `--consumers`, `--orders-per-producer`, `--queue-capacity` are all CLI flags. Prints throughput and, per symbol, resting order count / best bid-ask / recent trades. |
| `tests/test_mpmc.cpp` | Queue correctness (no loss/duplication under real contention, `close()` semantics) + concurrent-submit correctness for `ShardedMatchingEngine` | Run under ThreadSanitizer during development; zero races found once test-harness-internal counter increments were moved out of worker threads (the shared `CHECK()` counter itself isn't thread-safe — see the comment in the test file). |

## Build & run

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/test_mpmc                 # correctness
./build/mpmc_matching_main --producers 8 --consumers 8 \
    --orders-per-producer 200000 --queue-capacity 8192
```

## What did *not* change

`OrderBook` (the actual price-time-priority book) is untouched:
`std::map` for the two price-ordered sides, `std::list` for FIFO
within a price level, `std::unordered_map` for O(1) cancel-by-id. That
data structure is correctness-critical and single-writer by design —
the multithreading here is about how *work gets to* an `OrderBook`
instance (many producers, many consumers, many book instances), not
about making one `OrderBook` internally thread-safe.
