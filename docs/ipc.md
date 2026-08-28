# IPC: mmap, Shared Memory, and the SPSC Queue

> **Version 2 note:** The same SPSC acquire/release design remains in use. Protocol v2 adds an execution-report queue, region-size handshake, counters, and advisory process heartbeats.

## Processes and virtual memory, briefly

Every process has its own virtual address space; by default, process
A cannot read or write process B's memory. `mmap()` is one of the
mechanisms Linux provides to deliberately punch a hole in that
isolation: it asks the kernel to map a range of the calling process's
virtual address space onto some backing object — here, a regular file
opened with `open()`. Passing `MAP_SHARED` means: if another process
maps the *same* file the same way, both mappings refer to the same
underlying physical pages, so writes by one are visible to reads by
the other (subject to the memory-ordering rules below — visibility
across CPU cores is a separate concern from visibility across the
page-table mapping).

**mmap() does not bypass the kernel.** It is a syscall that asks the
kernel to set up page-table entries. After the call returns, ordinary
load/store instructions from user space touch that memory directly —
there is no per-access syscall — but:

- the *first* touch of a given page can still take a **page fault**,
  handled by the kernel, which populates the page table entry (and,
  for a fresh file-backed mapping, may need to read the page from the
  backing store);
- the CPU still performs **virtual-to-physical address translation**
  on every access via the TLB/page tables, exactly as for any other
  memory;
- **CPU caches still matter** — a shared-memory write from another
  core still has to become visible through the normal cache-coherency
  protocol (see "false sharing" below); mmap doesn't change physics.

What mmap **does** buy you, compared to IPC via a pipe or socket, is
that after the mapping is set up there is no per-message copy into a
kernel buffer and back out, and no serialization/deserialization step
— messages are read and written in place, by both processes, as plain
memory. That is a real latency and throughput win at high message
rates. It is not, by itself, a guarantee of any particular latency
number, and "the entire path is zero-copy" would be an overstatement
— see "Zero-copy, precisely" below.

## Why processes can't just store pointers in shared memory

Each process independently calls `mmap()` on the same file. The kernel
is free to place that mapping at *whatever virtual address happens to
be convenient* in each process — there is no guarantee, and in
practice with ASLR it is common, that process A and process B get
different base addresses for the same shared file. So if process A
computes `Order* p = &shared->some_order;` and writes the raw bit
pattern of `p` into the shared region, that pointer is only meaningful
in A's address space. Process B reading those bytes back as a pointer
and dereferencing it reads garbage — or, worse, some unrelated valid
memory in B's own address space, silently corrupting something.

The fix used throughout this project: **never store a pointer in
shared memory.** `SharedRegion` (`include/shared_types.hpp`) contains
only:

- `std::atomic<...>` scalar fields (fixed-size, no indirection),
- `SPSCQueue<T, Capacity>`, whose only storage is a `std::array<T,
  Capacity>` **embedded by value**, plus two atomic index counters —
  the "pointer" into the queue is an integer offset (`index(counter)`
  computed with a bitmask), not a `T*`.

Every process computes its own local address for `shared->...` by
adding an offset to *its own* mapping's base address (that's exactly
what `reinterpret_cast<SharedRegion*>(region.data())` does) — the only
thing that ever crosses the process boundary through the shared bytes
themselves is plain data and integer indices.

## Shared-memory layout

```
SharedRegion
├── SharedHeader          magic, version, state (NOT_READY/READY),
│                         producer_done, engine_done, best_bid, best_ask
├── SPSCQueue<OrderMsg,N>  order_queue  (feed_handler -> matching_engine)
└── SPSCQueue<Trade,N>     trade_queue  (matching_engine -> market_viewer)
```

Every member type here is trivially copyable or an atomic of a
trivially copyable type — `static_assert`s in `order.hpp`/`trade.hpp`
enforce this for `Order`/`Trade` at compile time. No `std::string`,
`std::vector`, `std::map`, or pointer-to-object ever appears in this
struct.

## Initialization handshake

Because `feed_handler` and `market_viewer` might be started before
`matching_engine_main` has finished creating and sizing the file, the
header carries an explicit `RegionState { NOT_READY, READY }`. The
creator placement-constructs the whole `SharedRegion` (so every
`std::atomic` inside it, including the two queues' indices, starts at
its documented initial value) and only *then* stores `READY` — with a
`memory_order_release` store. Both other processes spin (`open()`
retried, then poll the atomic) until they observe `state == READY` via
a `memory_order_acquire` load. The acquire/release pairing here
guarantees that once a process observes `READY`, it also sees every
write the creator made *before* that store — i.e., a fully-
initialized, non-garbage `SharedRegion`. This is the same
acquire/release handshake pattern used inside the SPSC queue itself
(see below), applied once at start-up instead of once per message.

## SPSC queue: why single-producer/single-consumer, why no mutex

A **general** multi-producer/multi-consumer concurrent queue needs to
handle multiple threads racing to claim the *same* slot — that
fundamentally requires either a lock or a more complex lock-free CAS
loop with retries. With exactly **one** producer and **one** consumer,
there is no such race: the producer is the *only* writer of `head_`
and the *only* thing that ever writes into "the next" slot; the
consumer is the *only* writer of `tail_`. Each side only ever *reads*
the other's counter. That means the whole algorithm can be built from
plain loads/stores plus two atomic counters — no mutex, no compare-
and-swap, no retry loop.

Avoiding a mutex here isn't just about avoiding lock/unlock overhead
in the uncontended case (which is already fairly cheap on Linux via
futexes) — it's about avoiding **the possibility of blocking at all**.
A mutex-protected queue can put the producer to sleep (or spin) behind
the consumer (or vice versa) via the OS scheduler, which introduces
scheduling latency and jitter that a well-tuned SPSC ring buffer
avoids entirely: both sides can make progress purely with CPU-local
atomic operations.

**We do not claim this always wins in absolute throughput** — see
`docs/performance.md` for the actual measured comparison against a
naive `std::mutex + std::deque` queue in this project, including a
case where the mutex queue was faster on the sandbox hardware used to
generate these numbers.

## Memory ordering: the acquire/release handshake

`try_push` (producer):
1. Read `head_` (`relaxed` — only the producer writes it, so no other
   thread's view of it needs synchronizing for this read).
2. Read `tail_` (`acquire`) to check for space. This pairs with the
   consumer's `release` store to `tail_`.
3. Write the payload into `buffer_[slot]` — a plain, non-atomic store.
4. Publish: `head_.store(next, release)`.

`try_pop` (consumer):
1. Read `tail_` (`relaxed`).
2. Read `head_` (`acquire`) to check for data. This is the other half
   of the pairing with the producer's step 4: because the producer's
   `head_` store was `release` and this load is `acquire` and it
   observes that value, the C++ memory model guarantees this thread
   now also sees the producer's plain write to `buffer_[slot]` from
   step 3 — even though that write itself used no atomics.
3. Read the payload from `buffer_[slot]`.
4. Publish: `tail_.store(next, release)` — the other half of the
   pairing used by the producer's step 2 on its *next* call.

This release/acquire "happens-before" edge is exactly what's needed:
the payload write must happen-before the consumer's read of it, and
the consumer's read must happen-before the producer is allowed to
reuse that slot. Nothing here needs a *total* order across all atomic
operations in the program, which is what `memory_order_seq_cst` would
additionally provide (and, on non-x86 architectures like ARM, at real
extra instruction cost) — so we deliberately use the weaker, cheaper
`acquire`/`release` throughout the queue.

## Cache lines, false sharing, and cache-line bouncing

Modern CPUs move memory between cores in fixed-size chunks called
**cache lines** (typically 64 bytes on current x86_64/arm64 parts).
Cache coherency protocols (e.g. MESI) track ownership of a line, not
of individual variables. If `head_` (written only by the producer) and
`tail_` (written only by the consumer) shared one cache line, then
every time either side updates its counter, the *other* side's next
read of that same line requires the coherency protocol to transfer the
line between cores — even though the two counters are logically
unrelated. This is **false sharing**, and the repeated forced transfer
under high update rates is **cache-line bouncing**; it can dominate
the cost of an otherwise cheap atomic counter update.

`SPSCQueue` addresses this with `alignas(kCacheLineSize)` on `head_`,
`tail_`, and the start of `buffer_`, so the producer-owned and
consumer-owned state live on separate lines. `kCacheLineSize` prefers
`std::hardware_destructive_interference_size` when the standard
library provides it, and otherwise falls back to a **documented
assumption of 64 bytes** — correct for essentially all current
x86_64 hardware and most current arm64 desktop/server parts, but *not*
a portable guarantee; unusual hardware should be checked explicitly
before relying on it. GCC currently gates the standard constant behind
an ABI opt-in and warns when it's used, because its value can vary
with `-mtune`; this project builds every translation unit with the
same flags, so that specific hazard doesn't apply here, but it matters
if this code is ever split into a shared library consumed by
independently-compiled binaries.

## Zero-copy, precisely

Within this project's IPC path, an order or trade is written into
shared memory exactly once by its producer and read directly from that
same memory by its consumer — there is no intermediate serialization
buffer, no `memcpy` into a socket send buffer, no kernel-buffer copy.
That is a legitimate "zero (application-level) copy within our IPC
design" claim.

That is **not** the same as claiming the whole system is "true
end-to-end zero-copy": this project has no network layer at all (see
the project scope notes in the README), and even within a single
machine, the CPU's cache hierarchy still moves the underlying cache
lines between cores when ownership changes — that's hardware-level
data movement, not an *application-level copy* that our code performs
or pays an allocation for, but it is still real data movement.

## Sanitizers and process boundaries

AddressSanitizer/UndefinedBehaviorSanitizer instrument the process
they're linked into; they have no visibility into a *different*
process's memory. `tests/test_ipc.cpp` uses `fork()` to test real
cross-process shared memory, which sanitizers handle structurally fine
(each forked process is sanitized independently), but ASan's leak
detector can misreport across a `fork()` inside a single test binary
(it isn't designed around short-lived forked children exiting via
`_exit()`), so the project's sanitizer test run disables leak
detection specifically for `test_ipc` (`ASAN_OPTIONS=detect_leaks=0`)
while keeping it enabled for the non-IPC suites. This is a testing-
harness consideration, not a statement about the shared-memory code's
correctness.
