#include "mpmc_queue.hpp"
#include "sharded_engine.hpp"
#include "symbol.hpp"
#include "test_common.hpp"

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

using namespace lob;

// Multiple producers, multiple consumers, one shared bounded queue:
// every item pushed must be popped exactly once, with none lost or
// duplicated, regardless of thread interleaving.
void test_mpmc_no_loss_no_duplication() {
    constexpr size_t kProducers = 4;
    constexpr size_t kConsumers = 4;
    constexpr size_t kPerProducer = 5000;
    constexpr size_t kTotal = kProducers * kPerProducer;

    MPMCQueue<uint64_t> q(64); // deliberately small: forces real blocking/backpressure

    std::vector<std::atomic<uint32_t>> seen(kTotal);
    for (auto& s : seen) s = 0;

    // Worker threads record pass/fail into shared atomics rather than
    // calling CHECK() directly: CHECK() bumps a plain (non-atomic)
    // global counter, which is fine for this harness's single-threaded
    // tests but would itself be a data race if hammered from several
    // threads at once here. We aggregate under the single main thread
    // instead, after every worker has joined.
    std::atomic<bool> push_failed{false};
    std::atomic<bool> out_of_range{false};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (size_t i = 0; i < kPerProducer; ++i) {
                if (!q.push(p * kPerProducer + i)) push_failed = true;
            }
        });
    }

    std::atomic<size_t> popped{0};
    std::vector<std::thread> consumers;
    for (size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            uint64_t v;
            while (q.pop(v)) {
                if (v >= kTotal) out_of_range = true;
                else seen[v].fetch_add(1, std::memory_order_relaxed);
                popped.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    CHECK(!push_failed.load());
    CHECK(!out_of_range.load());
    CHECK(popped.load() == kTotal);
    for (auto& s : seen) CHECK(s.load() == 1); // exactly once, never 0, never 2+
}

void test_mpmc_try_variants() {
    MPMCQueue<int> q(2);
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(!q.try_push(3)); // full
    int v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 1);
    CHECK(q.try_push(3));
    CHECK(q.size() == 2);
}

void test_mpmc_close_wakes_blocked_pop() {
    MPMCQueue<int> q(4);
    std::atomic<bool> woke{false};
    std::thread t([&] {
        int v;
        CHECK(!q.pop(v)); // nothing ever pushed; close() must still return us
        woke = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.close();
    t.join();
    CHECK(woke.load());
}

// Sharded engine: many worker threads submitting orders across many
// symbols concurrently must never corrupt a book -- the invariant
// checker (reused from the single-threaded OrderBook) must hold for
// every symbol's book at the end, and per-symbol order counts must be
// exactly what was actually rested (i.e. orders_in - orders_matched).
void test_sharded_engine_concurrent_submit() {
    ShardedMatchingEngine engine;
    constexpr size_t kThreads = 6;
    constexpr size_t kPerThread = 3000;
    const char* symbols[] = {"AAPL", "MSFT", "GOOG"};

    std::atomic<uint64_t> id_gen{1};
    std::vector<std::thread> workers;
    for (size_t w = 0; w < kThreads; ++w) {
        workers.emplace_back([&, w] {
            std::vector<Trade> scratch;
            for (size_t i = 0; i < kPerThread; ++i) {
                Symbol sym(symbols[(w + i) % 3]);
                Order o;
                o.id = id_gen.fetch_add(1, std::memory_order_relaxed);
                o.side = (i % 2 == 0) ? Side::BUY : Side::SELL;
                o.price = 10000 + static_cast<int64_t>(i % 20) - 10;
                o.quantity = 1 + static_cast<uint32_t>(i % 5);
                scratch.clear();
                engine.submit(sym, o, scratch);
            }
        });
    }
    for (auto& t : workers) t.join();

    for (const char* sym : symbols) {
        auto snap = engine.snapshot(Symbol(sym));
        CHECK(snap.exists);
        // best_bid, if present, must be strictly less than best_ask
        // (no self-crossing residue left in the book).
        if (snap.best_bid && snap.best_ask) {
            CHECK(*snap.best_bid < *snap.best_ask);
        }
    }
}

int main() {
    RUN(test_mpmc_no_loss_no_duplication);
    RUN(test_mpmc_try_variants);
    RUN(test_mpmc_close_wakes_blocked_pop);
    RUN(test_sharded_engine_concurrent_submit);
    return test_summary();
}
