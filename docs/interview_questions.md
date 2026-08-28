# Interview Questions & Answers

> **Version 2 note:** The original answers remain useful for the baseline structures. For advanced order semantics, deterministic partitioning, risk, and recovery, also study [`order_semantics.md`](order_semantics.md), [`partitioned_engine.md`](partitioned_engine.md), and [`persistence.md`](persistence.md).

Grounded in this project's actual design and measured behavior — see
`docs/order_book.md`, `docs/ipc.md`, `docs/performance.md` for the
longer versions of several of these answers.

## Order Book

**1. Why price-time priority instead of some other matching rule?**
It's the standard, easy-to-reason-about, "fair" discipline used by
most real continuous limit order books: the best price always wins,
and among equal prices, whoever committed capital first is served
first. It also gives a deterministic, reproducible fill sequence for a
given order stream, which is essential for testing and for traders to
reason about.

**2. Why `std::map` for price levels instead of something fancier?**
Per the project's own stated priorities — correctness first, then
testing, then measurement, then optimization — `std::map` (a red-black
tree) gives correct O(log P) insert/erase/best-of behavior for free
with no custom allocator or intrusive bookkeeping, and is easy to
verify. A flat array indexed by tick or an intrusive skip list would
be faster but adds real complexity; see `docs/performance.md`
Experiment A for measured evidence of where `std::map`'s cost actually
shows up (tail latency on wide, low-crossing books).

**3. How does partial matching work?**
`MatchingEngine::submitOrder` loops while the incoming order still has
quantity and the opposite side's best price still crosses; each
iteration matches against the *front* (oldest) order at the best
price for `min(incoming_remaining, resting_quantity)`, calls
`OrderBook::reduceFront` to shrink or remove that resting order, and
decrements the incoming order's remaining quantity. Whatever is left
after the loop (possibly the whole order, possibly none of it) is
inserted into the book.

**4. How does cancellation work, and what's its complexity?**
An `unordered_map<order_id, {side, price, list-iterator}>` lets
`cancelOrder` jump straight to the order instead of scanning the book:
O(1) average hash lookup + O(1) list erase, plus O(log P) only if that
was the last order at its price level (to erase the empty level from
the price map). See `docs/order_book.md` for the full argument for why
`std::map`/`std::list` iterator stability is what makes this safe.

**5. What is the complexity of `addOrder`, `bestBid`, `depth`?**
`addOrder`: O(log P) for the map insert/lookup, O(1) to append to the
level's FIFO list. `bestBid`/`bestAsk`: O(1) — `map::begin()`.
`depth`: O(1) — `map::size()`.

## mmap

**6. What does `mmap()` actually do?**
Asks the kernel to map a range of the calling process's virtual
address space onto a backing object (here, a regular file). After that
call returns, ordinary loads/stores touch that memory directly; the
kernel doesn't intermediate individual accesses, but it does still
manage the underlying page table and can take page faults on first
touch of a page.

**7. What does `MAP_SHARED` mean, as opposed to `MAP_PRIVATE`?**
`MAP_SHARED` means writes are visible to other processes that map the
same file the same way (and are, eventually, written back to the
backing file). `MAP_PRIVATE` gives copy-on-write semantics — writes
are private to that mapping and never visible to other mappers or
written back. IPC via shared memory requires `MAP_SHARED`.

**8. Why use mmap for IPC instead of, say, a Unix domain socket?**
Once the mapping is established, both processes can read/write the
shared region with plain memory instructions — no per-message syscall,
kernel-buffer copy, or serialization step, which matters a lot at high
message rates. The tradeoff is that you give up the socket's built-in
framing/backpressure/blocking-read semantics and have to build your
own synchronization (here: the SPSC queue's atomics) and readiness
protocol (here: the `RegionState` header field) by hand.

**9. Can both processes map the region at different virtual
addresses? What does that imply?**
Yes — nothing guarantees process A and process B get the same base
address for the same file (ASLR makes this the common case). It
implies you can never store a raw pointer *into the shared region* in
the shared region itself: only offsets, indices, or values that don't
depend on either process's mapping address. See Q10.

**10. Why can't raw pointers be stored in shared memory?**
A pointer written by process A encodes an address in *A's* address
space. If process B reads that same bit pattern and dereferences it,
it's interpreting a number that has no defined relationship to B's own
mapping — best case it's an invalid address and crashes, worst case it
happens to be a valid address in B's space and silently corrupts
unrelated memory. This project's `SharedRegion` only ever stores POD
values and lets the SPSC queue's internal array indices stand in for
"pointers" (an index is process-independent; a pointer isn't).

## SPSC

**11. Why SPSC specifically, rather than a general MPMC queue?**
Because there really is exactly one producer and one consumer on each
queue in this pipeline (`feed_handler -> matching_engine`,
`matching_engine -> market_viewer`). Exploiting that lets the whole
algorithm avoid CAS retry loops and the extra synchronization a
general multi-producer/multi-consumer queue needs to arbitrate between
racing writers/readers of the same slot.

**12. Why no mutex?**
With a single writer for each of `head_`/`tail_`, there's no race to
protect — a mutex would add lock/unlock overhead and the *possibility*
of one side blocking behind OS scheduling of the other, which a
pure-atomics design avoids. (Measured counterpoint: `docs/
performance.md` Experiment B found a naive mutex+deque queue actually
faster in absolute throughput on this project's single-core benchmark
hardware — the "why" is discussed there.)

**13. Why atomics, and why specifically not `volatile`?**
`volatile` in C++ only prevents the compiler from eliding or
reordering accesses to that variable *at the compiler level*; it says
nothing about the CPU's own out-of-order execution or about
inter-thread visibility/ordering guarantees. `std::atomic` is what
actually gives you a defined cross-thread happens-before relationship
via specified memory orderings, which is what correctness here depends
on.

**14. What does acquire/release actually guarantee?**
A `release` store and a later `acquire` load *of the same atomic
variable, observing that store's value* establish a happens-before
edge: everything the releasing thread did before the store becomes
visible to the acquiring thread after the load. It's weaker (and
cheaper) than `seq_cst`, which additionally guarantees a single total
order across *all* seq_cst operations program-wide — a guarantee this
algorithm doesn't need. See `docs/ipc.md` for the exact push/pop
walkthrough.

**15. What happens when the queue is full? Empty?**
`try_push` returns `false` without touching `head_` or overwriting any
slot when `head_ - tail_ >= Capacity`. `try_pop` returns `false`
without touching `tail_` when `head_ == tail_`. Neither ever silently
overwrites unread data or returns stale/garbage data — both are
exercised directly in `tests/test_spsc.cpp`.

**16. What is false sharing, concretely, in this queue?**
If `head_` and `tail_` lived in the same cache line, the producer's
writes to `head_` would force the consumer's cache line holding
`tail_` to be invalidated (and vice versa) on essentially every
operation, even though the two counters have no logical dependency —
"false" in the sense that the sharing is an artifact of memory layout,
not of the algorithm. `alignas(kCacheLineSize)` on each atomic avoids
this by construction.

**17. Why a power-of-two capacity?**
So the slot index can be computed with `counter & (Capacity - 1)`
instead of `counter % Capacity` — a bitmask is cheaper than a general
modulo and, more importantly here, is what lets the "full" check
(`head - tail >= Capacity`) work correctly with wrap-around unsigned
arithmetic without extra bookkeeping.

## Performance

**18. Why measure p99 (and p99.9) instead of just the average?**
An average hides tail behavior — a system that's fast 99% of the time
and catastrophically slow 1% of the time can still have a great
average. In trading systems specifically, the tail is often where risk
lives (a late fill/cancel during a fast market is disproportionately
costly). This project's own Experiment A is a direct illustration: the
mostly-non-crossing workload's *average* (461–705 ns) looks only
moderately worse than mostly-crossing's, but its **p99.9 is ~20 µs
versus ~0.5 µs** — a >30x difference the mean alone would hide.

**19. What is cache locality, and where did it show up in this
project's own numbers?**
Keeping data that's accessed together physically close in memory (and
therefore likely to already be in a fast CPU cache rather than needing
a slow main-memory fetch). `docs/performance.md` Experiment A shows it
directly: the workload that keeps a larger resting order book (more
`std::map` price levels, more `std::list` nodes) has measurably worse
tail latency than the workload that keeps the book small, because more
of its working set falls out of cache.

**20. What caused the tail latency actually observed in this
project's benchmarks?**
Two distinct, both-measured causes: (a) larger/colder data structures
under low-crossing workloads (see Q19), and (b) OS scheduler
preemption/CPU-quota throttling on the shared single-core sandbox used
to generate these numbers, which shows up as occasional very large
`max` samples and, in the strict-alternation ping-pong benchmarks,
dominates the whole distribution (`docs/performance.md`, "A note on
scheduling artifacts").

**21. How would you profile this application further?**
`perf stat`/`perf record` for CPU cycles, cache-miss rates, and branch
mispredictions per benchmark run; `perf mem`/cachegrind specifically
to confirm the cache-locality hypothesis in Q19 rather than just
inferring it from workload shape; and, for the SPSC queue specifically,
hardware performance counters for cache-coherency traffic
(e.g. `c2c` / cross-core snoop events) to directly quantify false
sharing rather than just reasoning about it structurally.

**22. What would you optimize next, and why that first?**
The `std::map`-based price levels (Q2/Q19/Q20) — it's the single
largest, most reproducible source of tail latency actually measured in
this project, and the fix (index by integer tick) is well-understood
and doesn't require redesigning anything else.

## HFT (and why this project is not one)

**23. Why single-thread the matching engine?**
An order book is inherently sequential — nearly every operation can
change best bid/ask, which nearly every other operation needs to read.
Making it thread-safe would mean locking around most operations
anyway, defeating the purpose of parallelism; real systems instead
typically dedicate one single-threaded matching engine per instrument
and get parallelism *across* instruments, not within one book.

**24. How could multiple instruments be scaled, starting from this
code?**
Give each instrument its own `MatchingEngine` + its own pair of SPSC
queues (or a partitioned shared-memory region), pin each engine
process/thread to its own CPU core, and route incoming orders to the
right instrument's queue by symbol before they ever reach a book —
each book stays single-threaded and lock-free internally, and
horizontal scale comes from running more of them.

**25. What would a real production HFT system add that this project
deliberately does not have?**
Kernel-bypass networking (e.g. DPDK or RDMA) for exchange connectivity
in addition to the shared-memory IPC used here for local
inter-process communication; hardware timestamping; a wire protocol
and network-facing feed handlers (this project's "feed handler" only
generates synthetic local test data — no real network I/O at all,
per the project's explicit scope); risk checks and kill switches;
persistence/recovery and audit logging; redundancy/failover; and much
more extensive testing against exchange simulators. See the README's
"Limitations" section for the full list of what was intentionally left
out.

**26. What is "kernel bypass," and does this project use it?**
Kernel bypass means user-space code talks to hardware (typically a
NIC) directly, without going through the OS networking stack, to
avoid syscall and kernel-buffer-copy overhead on the network path —
DPDK and RDMA are common examples. This project has **no networking at
all** (by explicit design — see the project scope), so there is
nothing to bypass; `mmap()`-based shared memory is a related but
distinct idea (bypassing *inter-process copying* on a single machine,
not bypassing the *network stack*), and `docs/ipc.md` is explicit that
mmap itself still goes through the kernel to establish the mapping.

**27. Why is this project explicitly NOT a production exchange?**
No networking, no persistence/durability, no risk controls, no
authentication, no multi-instrument routing, a `std::map`-based book
that's correctness-optimized rather than latency-optimized, a matching
engine and feed handler that talk to synthetic/local data only, and no
redundancy or failover story. It's scoped, on purpose, to make the
order-book/matching/IPC/lock-free-queue/memory-ordering concepts
clearly visible and testable in ~2,400 lines of code — see the
project brief's explicit "NOT a production exchange" statement.
