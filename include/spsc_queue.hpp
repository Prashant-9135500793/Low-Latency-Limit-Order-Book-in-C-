#pragma once
// spsc_queue.hpp
//
// Single-Producer Single-Consumer lock-free ring buffer.
//
// DESIGN
// ------
// A fixed-size circular array of T plus two monotonically increasing
// 64-bit counters: `head_` (next write slot, producer-owned) and
// `tail_` (next read slot, consumer-owned). Capacity MUST be a power
// of two so we can map a counter to a slot with a cheap bitmask
// instead of a modulo.
//
// This queue is a POD-like aggregate (std::array + two atomics, no
// owning pointers) so it can be placed directly inside a memory-mapped
// shared-memory region and used across process boundaries: the
// producer process and the consumer process each mmap the same file
// and reinterpret the same bytes as SPSCQueue<T,N>. Because the type
// holds no pointers into its own address space (only indices/array
// storage), it remains valid no matter which virtual address each
// process happens to map the region at.
//
// MEMORY ORDERING
// ----------------
// Producer (try_push):
//   1. Compute next head slot from head_.load(relaxed) — only the
//      producer ever writes head_, so relaxed is safe for its own read.
//   2. Check against tail_.load(acquire) to see if there is space.
//      Acquire here syncs-with the consumer's release store to tail_,
//      guaranteeing we see the consumer's completed reads of the slot
//      we are about to overwrite.
//   3. Write the payload into buffer_[slot] (plain, non-atomic store).
//   4. Publish by head_.store(next, release). The release pairs with
//      the consumer's acquire load of head_ and guarantees that the
//      payload write in step 3 is visible to the consumer *before* it
//      observes the updated head_.
//
// Consumer (try_pop):
//   1. Read tail_.load(relaxed) — only the consumer writes tail_.
//   2. Read head_.load(acquire) to see if an item is available. This
//      acquire syncs-with the producer's release store to head_,
//      making the producer's payload write from step 3 above visible
//      here before we read buffer_[slot].
//   3. Read the payload from buffer_[slot].
//   4. Publish by tail_.store(next, release), which is what the
//      producer's acquire load of tail_ (step 2 above) will observe,
//      completing the "slot is free again" handshake.
//
// This is the classic release/acquire "happens-before" handshake: it
// is strictly weaker (and cheaper on most architectures, notably
// non-x86) than std::memory_order_seq_cst, but still gives exactly
// the ordering guarantees this algorithm needs. We deliberately do
// NOT use seq_cst everywhere — that would add unnecessary global
// ordering constraints (and, on architectures like ARM, real extra
// fence instructions) that this single-producer/single-consumer
// protocol does not require.
//
// CACHE-LINE AWARENESS / FALSE SHARING
// -------------------------------------
// If head_ and tail_ sat in the same cache line, every producer push
// (which writes head_) would invalidate the cache line that the
// consumer just read tail_ from (and vice versa), forcing the cache
// coherency protocol (e.g. MESI) to repeatedly transfer ownership of
// that line between CPU cores — "cache-line bouncing" — even though
// the two counters are logically independent. We separate them with
// alignas(kAlign) so each lives on its own cache line and the two
// threads/processes stop fighting over shared cache-coherency state.
//
// We use std::hardware_destructive_interference_size where available.
// It is not guaranteed portable/stable across all platforms (ABI
// concerns are why it needs opt-in on some compilers), so we fall
// back to a documented, explicit 64 bytes -- correct for essentially
// all current x86_64 and most arm64 desktop/server parts, but not a
// universal law of nature. If you port this to unusual hardware,
// verify the real destructive-interference size.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace lob {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
inline constexpr size_t kCacheLineSize = 64; // documented platform assumption
#endif

template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue element type must be trivially copyable "
                  "(required for shared-memory / cross-process use)");

public:
    SPSCQueue() noexcept : head_(0), tail_(0) {}

    // Non-copyable, non-movable: this object's identity IS its memory
    // address range (possibly inside a shared-memory mapping); copying
    // or moving it would be meaningless.
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Producer side. Returns false if the queue is full.
    bool try_push(const T& item) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            return false; // full
        }
        buffer_[index(head)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the queue is empty.
    bool try_pop(T& out) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false; // empty
        }
        out = buffer_[index(tail)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate, racy w.r.t. the other side by design -- fine for
    // monitoring/logging, not for correctness decisions.
    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    bool full() const noexcept {
        return head_.load(std::memory_order_acquire) -
                   tail_.load(std::memory_order_acquire) >=
               Capacity;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(head_.load(std::memory_order_acquire) -
                                    tail_.load(std::memory_order_acquire));
    }

    static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t index(uint64_t counter) noexcept {
        return static_cast<size_t>(counter & (Capacity - 1));
    }

    alignas(kCacheLineSize) std::atomic<uint64_t> head_; // producer-owned
    alignas(kCacheLineSize) std::atomic<uint64_t> tail_; // consumer-owned
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

} // namespace lob
