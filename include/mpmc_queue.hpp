#pragma once
// mpmc_queue.hpp
//
// Bounded Multi-Producer Multi-Consumer queue.
//
// WHY THIS EXISTS ALONGSIDE spsc_queue.hpp
// -----------------------------------------
// spsc_queue.hpp is a lock-free ring buffer for exactly one producer
// thread and exactly one consumer thread (e.g. one feed-handler
// process talking to one matching-engine process over shared memory).
// That single-writer/single-reader assumption is what lets it avoid
// locks entirely.
//
// The moment you have SEVERAL producer threads (e.g. several feed
// simulators/gateways generating orders concurrently) and/or SEVERAL
// consumer/worker threads (a thread pool draining the queue), that
// assumption breaks: two producers racing on `head_` with only a
// relaxed load-then-store is a lost-update data race, not just a
// performance bug. An MPMC queue needs the push/pop sequence itself
// (reserve a slot, write, publish) to be atomic across *all*
// producers and *all* consumers, which in the general case means
// either a mutex (simple, correct, what we do here) or a much more
// involved lock-free algorithm (e.g. Vyukov's bounded MPMC queue,
// with a per-slot sequence number to let multiple producers/consumers
// claim distinct slots via CAS). We use the mutex + condition_variable
// version: it is easy to reason about, it is fast enough that the
// bottleneck in this project is the matching engine's book mutations
// rather than the queue, and getting a lock-free MPMC ring buffer
// *correct* (ABA-safe, no lost wakeups) is a substantial project of
// its own -- not something to fake for an "educational" codebase.
//
// SEMANTICS
// ---------
//   push()/pop()          block the calling thread when full/empty.
//   try_push()/try_pop()  never block; return false instead.
//   close()               wakes every blocked thread; after close(),
//                          push() fails (returns false) and pop()
//                          continues to drain remaining items, then
//                          returns false once the queue is empty --
//                          this is the standard "poison the queue,
//                          let consumers finish draining" shutdown
//                          idiom for a thread-pool pipeline.
//
// This queue is intentionally NOT trivially-copyable-constrained
// (unlike SPSCQueue) because it never crosses a process boundary --
// it lives entirely in one process's heap, guarded by a mutex, so
// there is no shared-memory/relocatable-bytes requirement on T.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace lob {

template <typename T>
class MPMCQueue {
public:
    explicit MPMCQueue(size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("MPMCQueue capacity must be greater than zero");
        }
    }

    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    // Blocks until there is space or the queue is closed. Returns
    // false if the queue was (or became) closed before space freed up.
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [&] { return closed_ || buffer_.size() < capacity_; });
        if (closed_) return false;
        buffer_.push_back(std::move(item));
        high_watermark_ = std::max(high_watermark_, buffer_.size());
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Non-blocking. Returns false if full or closed.
    bool try_push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || buffer_.size() >= capacity_) return false;
            buffer_.push_back(std::move(item));
            high_watermark_ = std::max(high_watermark_, buffer_.size());
        }
        not_empty_.notify_one();
        return true;
    }

    // Blocks until an item is available or the queue is closed AND
    // drained. Returns false only in the latter case (real shutdown).
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || !buffer_.empty(); });
        if (buffer_.empty()) return false; // closed and drained
        out = std::move(buffer_.front());
        buffer_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // Non-blocking. Returns false if empty right now.
    bool try_pop(T& out) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (buffer_.empty()) return false;
            out = std::move(buffer_.front());
            buffer_.pop_front();
        }
        not_full_.notify_one();
        return true;
    }

    // Wakes every thread blocked in push()/pop(). Producers see the
    // queue as permanently full; consumers keep draining what's left,
    // then see it as permanently empty. Idempotent.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    }

    size_t capacity() const { return capacity_; }

    size_t highWatermark() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_watermark_;
    }

private:
    // std::deque, not std::vector/std::array: we need O(1) push at the
    // back and O(1) pop at the front simultaneously, and (unlike
    // SPSCQueue's ring buffer) capacity is a soft bound checked under
    // the lock rather than something we need packed into a fixed-size
    // array -- a deque gives us that FIFO shape directly without
    // hand-rolling index wraparound.
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> buffer_;
    size_t capacity_;
    size_t high_watermark_ = 0;
    bool closed_ = false;
};

} // namespace lob
