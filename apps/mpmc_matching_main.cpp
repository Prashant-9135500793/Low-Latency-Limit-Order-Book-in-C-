// mpmc_matching_main.cpp
//
// Legacy lock-based multi-producer/multi-consumer comparison demo built
// on top of one shared MPMCQueue + ShardedMatchingEngine. It is data-race
// safe, but a shared worker pool does not guarantee that same-symbol
// commands acquire a shard lock in the order they were popped. The
// recommended deterministic architecture is partitioned_matching_main.
//
//   [producer thread 0]  \                                  / [worker thread 0]
//   [producer thread 1]   >---> MPMCQueue<SymbolOrder> --->  - [worker thread 1]
//   [producer thread ..]  /                                  \ [worker thread ..]
//
// Each producer thread independently generates random LIMIT orders
// across a fixed universe of symbols and pushes them into ONE shared
// bounded queue (push() blocks if the queue is momentarily full --
// natural backpressure). Each consumer/worker thread pops from that
// SAME queue and hands the order to ShardedMatchingEngine::submit(),
// which internally routes to the correct per-symbol book under that
// symbol's shard lock (see sharded_engine.hpp for why that's safe and
// still parallel across symbols).
//
// This is the "several feed sources, several matching workers, many
// instruments" shape; matching_engine_main.cpp remains the minimal
// single-producer/single-consumer/single-instrument program the rest
// of the docs walk through.
//
// Usage: mpmc_matching_main [--producers N] [--consumers N]
//                            [--orders-per-producer N] [--queue-capacity N]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "mpmc_queue.hpp"
#include "sharded_engine.hpp"
#include "symbol.hpp"

using namespace lob;
using namespace std::chrono_literals;

namespace {

constexpr const char* kSymbols[] = {"AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA"};
constexpr size_t kSymbolCount = sizeof(kSymbols) / sizeof(kSymbols[0]);

struct Args {
    size_t producers = 4;
    size_t consumers = 4;
    size_t orders_per_producer = 20000;
    size_t queue_capacity = 4096;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](size_t& out) { if (i + 1 < argc) out = std::stoul(argv[++i]); };
        if (std::strcmp(argv[i], "--producers") == 0) next(a.producers);
        else if (std::strcmp(argv[i], "--consumers") == 0) next(a.consumers);
        else if (std::strcmp(argv[i], "--orders-per-producer") == 0) next(a.orders_per_producer);
        else if (std::strcmp(argv[i], "--queue-capacity") == 0) next(a.queue_capacity);
    }
    return a;
}

// One producer thread: generates random orders for a random mix of
// symbols and pushes them into the shared queue. Uses its own PRNG
// (seeded from thread id) so producers don't contend on RNG state.
void producerThread(size_t producer_id, size_t order_count, std::atomic<uint64_t>& id_gen,
                     MPMCQueue<SymbolOrder>& queue, std::atomic<size_t>& produced) {
    std::mt19937_64 rng(0xC0FFEE'0000ull + producer_id);
    std::uniform_int_distribution<size_t> symbol_dist(0, kSymbolCount - 1);
    std::uniform_int_distribution<int64_t> price_dist(9900, 10100); // ticks around $99-$101
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    std::bernoulli_distribution side_dist(0.5);

    for (size_t i = 0; i < order_count; ++i) {
        SymbolOrder so;
        so.symbol = kSymbols[symbol_dist(rng)];
        so.order.id = id_gen.fetch_add(1, std::memory_order_relaxed);
        so.order.side = side_dist(rng) ? Side::BUY : Side::SELL;
        so.order.type = OrderType::LIMIT;
        so.order.price = price_dist(rng);
        so.order.quantity = qty_dist(rng);

        // Blocking push: if the queue is full, this producer simply
        // waits -- correct, natural backpressure for a bounded queue
        // shared by several producers.
        if (!queue.push(so)) break; // queue was closed underneath us
        produced.fetch_add(1, std::memory_order_relaxed);
    }
}

// One consumer/worker thread: drains the shared queue and applies
// each order to the sharded engine until the queue is closed and
// empty.
void consumerThread(size_t worker_id, MPMCQueue<SymbolOrder>& queue,
                     ShardedMatchingEngine& engine, std::atomic<size_t>& consumed,
                     std::atomic<size_t>& trades_generated) {
    std::vector<Trade> scratch;
    SymbolOrder so;
    while (queue.pop(so)) {
        scratch.clear();
        const size_t n = engine.submit(so.symbol, so.order, scratch);
        trades_generated.fetch_add(n, std::memory_order_relaxed);
        consumed.fetch_add(1, std::memory_order_relaxed);
    }
    std::printf("[worker %zu] exiting: queue closed and drained\n", worker_id);
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    std::printf(
        "[legacy MPMC comparison; use partitioned_matching_main for deterministic "
        "same-symbol sequencing]\n");
    std::printf(
        "mpmc_matching_main: %zu producer thread(s), %zu consumer thread(s), "
        "%zu orders/producer, queue capacity %zu, %zu instruments\n",
        args.producers, args.consumers, args.orders_per_producer, args.queue_capacity,
        kSymbolCount);

    MPMCQueue<SymbolOrder> queue(args.queue_capacity);
    ShardedMatchingEngine engine;

    std::atomic<uint64_t> id_gen{1};
    std::atomic<size_t> produced{0};
    std::atomic<size_t> consumed{0};
    std::atomic<size_t> trades_generated{0};

    const auto start = std::chrono::steady_clock::now();

    // Launch consumers first so they're ready to drain as soon as
    // producers start publishing.
    std::vector<std::thread> consumers;
    for (size_t c = 0; c < args.consumers; ++c) {
        consumers.emplace_back(consumerThread, c, std::ref(queue), std::ref(engine),
                                std::ref(consumed), std::ref(trades_generated));
    }

    std::vector<std::thread> producers;
    for (size_t p = 0; p < args.producers; ++p) {
        producers.emplace_back(producerThread, p, args.orders_per_producer, std::ref(id_gen),
                                std::ref(queue), std::ref(produced));
    }

    for (auto& t : producers) t.join();
    // All producers are done publishing: close() wakes any blocked
    // consumers and tells them to stop once the queue drains, rather
    // than waiting on an empty queue forever.
    queue.close();
    for (auto& t : consumers) t.join();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();

    std::printf("\n=== summary ===\n");
    std::printf("produced=%zu consumed=%zu trades=%zu elapsed=%.3fs (%.0f orders/sec)\n",
                produced.load(), consumed.load(), trades_generated.load(), seconds,
                static_cast<double>(consumed.load()) / std::max(seconds, 1e-9));

    std::printf("\n=== per-symbol book state ===\n");
    for (const char* sym : kSymbols) {
        Symbol s(sym);
        auto snap = engine.snapshot(s);
        auto recent = engine.tradeLog().recent(s, 3);
        std::printf("%-6s  resting_orders=%-6zu bid=%-8s ask=%-8s  last_trades=%zu",
                    sym, snap.order_count,
                    snap.best_bid ? std::to_string(*snap.best_bid).c_str() : "-",
                    snap.best_ask ? std::to_string(*snap.best_ask).c_str() : "-",
                    engine.tradeLog().count(s));
        if (!recent.empty()) {
            std::printf("  (most recent: px=%ld qty=%u)", static_cast<long>(recent.back().price),
                        recent.back().quantity);
        }
        std::printf("\n");
    }

    return 0;
}
