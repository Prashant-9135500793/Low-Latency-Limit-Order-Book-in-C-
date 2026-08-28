// benchmark_spsc.cpp
//
// Compares lock-free SPSCQueue throughput/latency against a naive
// std::mutex + std::deque queue, across several message counts. Also
// measures ping-pong round-trip latency (two queues, one thread bounces
// a token back) as a per-message latency proxy.
//
// We deliberately do NOT assume the lock-free queue always wins --
// see docs/performance.md for the actual measured numbers and
// discussion of when a mutex queue can be competitive (e.g. very low
// contention, or when OS scheduling noise dominates anyway).

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "benchmark.hpp"
#include "spsc_queue.hpp"
#include "timestamp.hpp"

using namespace lob;

namespace {

// ---- naive mutex-protected queue for comparison ----
template <typename T>
class MutexQueue {
public:
    void push(const T& v) {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(v);
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex m_;
    std::deque<T> q_;
};

double throughputSPSC(uint64_t n) {
    SPSCQueue<uint64_t, 1u << 16> q;
    auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        for (uint64_t i = 0; i < n; ++i) {
            while (!q.try_push(i)) std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        uint64_t v, received = 0;
        while (received < n) {
            if (q.try_pop(v)) ++received;
        }
    });
    producer.join();
    consumer.join();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

double throughputMutex(uint64_t n) {
    MutexQueue<uint64_t> q;
    auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        for (uint64_t i = 0; i < n; ++i) q.push(i);
    });
    std::thread consumer([&] {
        uint64_t v, received = 0;
        while (received < n) {
            if (q.try_pop(v)) ++received;
        }
    });
    producer.join();
    consumer.join();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

// Ping-pong round trip: thread A pushes a timestamp into q_out, thread
// B pops it, immediately pushes it back into q_in, A pops and records
// (now - sent) as the round trip time. Repeated n times, sequentially
// (each round trip must finish before the next starts) so this
// measures latency, not throughput.
bench::LatencyStats pingPongSPSC(uint64_t n) {
    SPSCQueue<uint64_t, 1024> q_out, q_in;
    std::vector<uint64_t> rtt;
    rtt.reserve(n);
    std::atomic<bool> stop{false};

    std::thread responder([&] {
        uint64_t v;
        while (!stop.load(std::memory_order_relaxed)) {
            if (q_out.try_pop(v)) {
                while (!q_in.try_push(v)) std::this_thread::yield();
            }
        }
    });

    busy_warmup(1'000'000);
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t t0 = now_ns();
        while (!q_out.try_push(t0)) std::this_thread::yield();
        uint64_t echoed;
        while (!q_in.try_pop(echoed)) std::this_thread::yield();
        const uint64_t t1 = now_ns();
        rtt.push_back(t1 - t0);
    }
    stop.store(true, std::memory_order_relaxed);
    responder.join();

    return bench::computeStats(rtt);
}

} // namespace

int main() {
    const std::vector<uint64_t> sizes = {1'000'000, 5'000'000, 10'000'000};

    std::printf("### Throughput: SPSCQueue vs mutex+deque queue ###\n");
    for (uint64_t n : sizes) {
        double t_spsc = throughputSPSC(n);
        bench::printThroughput("SPSCQueue (n=" + std::to_string(n) + ")", n, t_spsc);
        double t_mutex = throughputMutex(n);
        bench::printThroughput("MutexQueue (n=" + std::to_string(n) + ")", n, t_mutex);
        std::printf("  speedup (mutex_time / spsc_time): %.2fx\n\n",
                    t_spsc > 0 ? t_mutex / t_spsc : 0.0);
    }

    std::printf("### Ping-pong round-trip latency (in-process, two SPSC queues) ###\n");
    // NOTE: strict-alternation ping-pong (each iteration must complete
    // before the next starts) is extremely sensitive to OS scheduling
    // granularity and, on machines/containers with a fractional CPU
    // quota (cgroup cpu.cfs_quota_us), can trigger periodic scheduler
    // throttling stalls that dominate the measured latency and have
    // nothing to do with the SPSC algorithm itself. We use a modest
    // iteration count here so the benchmark completes reliably in
    // constrained environments; see docs/performance.md for a
    // discussion of what was actually observed in this run.
    auto stats = pingPongSPSC(3000);
    bench::printStats("spsc_ping_pong_rtt", stats);
    std::printf("  (one-way latency is approximately rtt / 2)\n");

    return 0;
}
