// partitioned_matching_main.cpp
//
// Recommended multi-instrument architecture:
//   many producers -> symbol partitioning -> one bounded queue and one
//   owner thread per shard -> one single-writer MatchingEngine per book.
//
// Unlike the legacy shared MPMC worker-pool demo, same-symbol commands
// cannot be popped by two workers and acquire a book lock out of order.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "partitioned_engine.hpp"

using namespace lob;

namespace {

struct Args {
    size_t producers = 4;
    size_t shards = 4;
    size_t orders_per_producer = 100'000;
    size_t queue_capacity = 4096;
    uint64_t seed = 42;
    bool advanced_orders = false;
};

uint64_t parseU64(const char* text, const char* flag) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || text[0] == '-') {
        std::fprintf(stderr, "invalid value for %s: %s\n", flag, text);
        std::exit(1);
    }
    return static_cast<uint64_t>(value);
}

size_t parseSize(const char* text, const char* flag) {
    const uint64_t value = parseU64(text, flag);
    if (value > std::numeric_limits<size_t>::max()) {
        std::fprintf(stderr, "value for %s exceeds size_t\n", flag);
        std::exit(1);
    }
    return static_cast<size_t>(value);
}

Args parseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (flag == "--producers") {
            args.producers = parseSize(next("--producers"), flag.c_str());
        } else if (flag == "--shards") {
            args.shards = parseSize(next("--shards"), flag.c_str());
        } else if (flag == "--orders-per-producer") {
            args.orders_per_producer =
                parseSize(next("--orders-per-producer"), flag.c_str());
        } else if (flag == "--queue-capacity") {
            args.queue_capacity = parseSize(next("--queue-capacity"), flag.c_str());
        } else if (flag == "--seed") {
            args.seed = parseU64(next("--seed"), flag.c_str());
        } else if (flag == "--advanced-orders") {
            args.advanced_orders = true;
        } else if (flag == "--help") {
            std::printf(
                "Usage: partitioned_matching_main [--producers N] [--shards N]\n"
                "       [--orders-per-producer N] [--queue-capacity N]\n"
                "       [--seed N] [--advanced-orders]\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
            std::exit(1);
        }
    }

    if (args.producers == 0 || args.orders_per_producer == 0 ||
        args.queue_capacity == 0 || args.shards == 0 ||
        (args.shards & (args.shards - 1)) != 0) {
        std::fprintf(stderr,
                     "producers/orders/queue must be non-zero and shards must be a "
                     "non-zero power of two\n");
        std::exit(1);
    }
    if (args.orders_per_producer >
        std::numeric_limits<uint64_t>::max() / args.producers) {
        std::fprintf(stderr, "total order count overflows uint64_t\n");
        std::exit(1);
    }
    return args;
}

Order generateOrder(uint64_t id, std::mt19937_64& rng, bool advanced) {
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int64_t> price_dist(9'950, 10'050);
    std::uniform_int_distribution<uint32_t> quantity_dist(1, 250);
    std::uniform_int_distribution<int> policy_dist(0, 99);

    Order order;
    order.id = id;
    order.side = side_dist(rng) == 0 ? Side::BUY : Side::SELL;
    order.type = OrderType::LIMIT;
    order.time_in_force = TimeInForce::GTC;
    order.price = price_dist(rng);
    order.quantity = quantity_dist(rng);

    if (!advanced) return order;

    const int policy = policy_dist(rng);
    if (policy < 5) {
        order.type = OrderType::MARKET;
        order.time_in_force = TimeInForce::IOC;
        order.price = 0;
    } else if (policy < 15) {
        order.time_in_force = TimeInForce::IOC;
    } else if (policy < 20) {
        order.time_in_force = TimeInForce::FOK;
    } else if (policy < 25) {
        order.flags = OrderFlags::POST_ONLY;
    }
    return order;
}

const char* printable(const std::optional<int64_t>& value, char* storage, size_t size) {
    if (!value) return "-";
    std::snprintf(storage, size, "%lld", static_cast<long long>(*value));
    return storage;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    const std::vector<Symbol> symbols = {
        Symbol("AAPL"), Symbol("MSFT"), Symbol("GOOG"), Symbol("AMZN"),
        Symbol("TSLA"), Symbol("NVDA"), Symbol("META"), Symbol("ORCL"),
    };

    const uint64_t total_orders =
        static_cast<uint64_t>(args.producers) * args.orders_per_producer;
    std::printf(
        "[partitioned] producers=%zu shards=%zu orders=%llu queue/shard=%zu "
        "mode=%s\n",
        args.producers, args.shards, static_cast<unsigned long long>(total_orders),
        args.queue_capacity, args.advanced_orders ? "advanced" : "GTC-limit");

    PartitionedMatchingEngine engine(args.shards, args.queue_capacity);
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> ingress_failed{false};
    std::vector<std::thread> producers;
    producers.reserve(args.producers);

    for (size_t producer_index = 0; producer_index < args.producers; ++producer_index) {
        producers.emplace_back([&, producer_index] {
            std::mt19937_64 rng(args.seed + producer_index * 0x9E3779B97F4A7C15ULL);
            std::uniform_int_distribution<size_t> symbol_dist(0, symbols.size() - 1);
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();

            const uint64_t id_base =
                static_cast<uint64_t>(producer_index) * args.orders_per_producer + 1;
            for (size_t i = 0; i < args.orders_per_producer; ++i) {
                const uint64_t id = id_base + i;
                const Symbol& symbol = symbols[symbol_dist(rng)];
                if (!engine.enqueue(symbol, generateOrder(id, rng, args.advanced_orders))) {
                    ingress_failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != args.producers) {
        std::this_thread::yield();
    }
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& producer : producers) producer.join();
    engine.waitUntilIdle();
    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end - begin).count();
    const PartitionedEngineStats stats = engine.stats();

    std::printf(
        "[partitioned] elapsed=%.6f s throughput=%.0f commands/s submitted=%llu "
        "processed=%llu accepted=%llu rejected=%llu trades=%llu\n",
        elapsed, elapsed > 0.0 ? static_cast<double>(stats.processed) / elapsed : 0.0,
        static_cast<unsigned long long>(stats.submitted),
        static_cast<unsigned long long>(stats.processed),
        static_cast<unsigned long long>(stats.accepted),
        static_cast<unsigned long long>(stats.rejected),
        static_cast<unsigned long long>(stats.trades));
    std::printf(
        "[partitioned] end-to-end latency (log2 histogram, upper-bound percentiles): "
        "min=%llu ns mean=%.1f ns p50<=%llu ns p95<=%llu ns p99<=%llu ns "
        "p99.9<=%llu ns max=%llu ns; queue high-water=%zu\n",
        static_cast<unsigned long long>(stats.end_to_end_latency.min_ns),
        stats.end_to_end_latency.mean_ns,
        static_cast<unsigned long long>(stats.end_to_end_latency.p50_ns),
        static_cast<unsigned long long>(stats.end_to_end_latency.p95_ns),
        static_cast<unsigned long long>(stats.end_to_end_latency.p99_ns),
        static_cast<unsigned long long>(stats.end_to_end_latency.p999_ns),
        static_cast<unsigned long long>(stats.end_to_end_latency.max_ns),
        stats.max_queue_high_watermark);

    bool invariants_ok = !ingress_failed.load(std::memory_order_acquire) &&
                         stats.submitted == total_orders &&
                         stats.processed == total_orders && stats.pending == 0;
    for (const Symbol& symbol : symbols) {
        const auto snapshot = engine.snapshot(symbol);
        char bid[32]{};
        char ask[32]{};
        std::printf(
            "  %-5s bid=%s ask=%s orders=%zu depth=%zu/%zu seq=%llu recent_trades=%zu\n",
            symbol.c_str(), printable(snapshot.best_bid, bid, sizeof(bid)),
            printable(snapshot.best_ask, ask, sizeof(ask)), snapshot.order_count,
            snapshot.buy_depth, snapshot.sell_depth,
            static_cast<unsigned long long>(snapshot.sequence),
            engine.tradeLog().count(symbol));
        invariants_ok = invariants_ok && engine.validate(symbol);
    }

    engine.shutdown();
    std::printf("[partitioned] validation=%s\n", invariants_ok ? "PASS" : "FAIL");
    return invariants_ok ? 0 : 2;
}
