// benchmark_partitioned.cpp
//
// End-to-end benchmark for the recommended symbol-partitioned pipeline.
// It reports throughput, bounded-queue pressure, and approximate latency
// percentiles. Results depend strongly on core count, CPU affinity, and
// whether the environment is throttled; this program prints facts only.

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
#include "symbol.hpp"

using namespace lob;

namespace {

struct Args {
    size_t orders = 500'000;
    size_t queue_capacity = 4096;
    uint64_t seed = 12345;
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
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", flag.c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (flag == "--orders") args.orders = parseSize(next(), flag.c_str());
        else if (flag == "--queue-capacity") {
            args.queue_capacity = parseSize(next(), flag.c_str());
        } else if (flag == "--seed") args.seed = parseU64(next(), flag.c_str());
        else if (flag == "--help") {
            std::printf("Usage: benchmark_partitioned [--orders N] "
                        "[--queue-capacity N] [--seed N]\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
            std::exit(1);
        }
    }
    if (args.orders == 0 || args.queue_capacity == 0) {
        std::fprintf(stderr, "orders and queue capacity must be non-zero\n");
        std::exit(1);
    }
    return args;
}

struct Input {
    Symbol symbol;
    Order order;
};

std::vector<Input> generateInputs(size_t count, uint64_t seed) {
    const Symbol symbols[] = {
        Symbol("AAPL"), Symbol("MSFT"), Symbol("GOOG"), Symbol("AMZN"),
        Symbol("TSLA"), Symbol("NVDA"), Symbol("META"), Symbol("ORCL"),
    };
    constexpr size_t symbol_count = sizeof(symbols) / sizeof(symbols[0]);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> symbol_dist(0, symbol_count - 1);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int64_t> price_dist(9'950, 10'050);
    std::uniform_int_distribution<uint32_t> quantity_dist(1, 200);

    std::vector<Input> inputs;
    inputs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Order order;
        order.id = static_cast<uint64_t>(i) + 1;
        order.side = side_dist(rng) == 0 ? Side::BUY : Side::SELL;
        order.price = price_dist(rng);
        order.quantity = quantity_dist(rng);
        inputs.push_back(Input{symbols[symbol_dist(rng)], order});
    }
    return inputs;
}

struct RunResult {
    size_t producers = 0;
    size_t shards = 0;
    double elapsed_seconds = 0.0;
    PartitionedEngineStats stats;
    bool valid = false;
};

RunResult runOnce(const std::vector<Input>& inputs, size_t producers, size_t shards,
                  size_t queue_capacity) {
    PartitionedMatchingEngine engine(shards, queue_capacity);
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(producers);

    for (size_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([&, producer] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t i = producer; i < inputs.size(); i += producers) {
                if (!engine.enqueue(inputs[i].symbol, inputs[i].order)) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != producers) std::this_thread::yield();
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    engine.waitUntilIdle();
    const auto end = std::chrono::steady_clock::now();

    RunResult result;
    result.producers = producers;
    result.shards = shards;
    result.elapsed_seconds = std::chrono::duration<double>(end - begin).count();
    result.stats = engine.stats();
    result.valid = !failed.load(std::memory_order_acquire) &&
                   result.stats.submitted == inputs.size() &&
                   result.stats.processed == inputs.size() && result.stats.pending == 0;

    const Symbol symbols[] = {
        Symbol("AAPL"), Symbol("MSFT"), Symbol("GOOG"), Symbol("AMZN"),
        Symbol("TSLA"), Symbol("NVDA"), Symbol("META"), Symbol("ORCL"),
    };
    for (const Symbol& symbol : symbols) result.valid = result.valid && engine.validate(symbol);
    engine.shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    const std::vector<Input> inputs = generateInputs(args.orders, args.seed);

    std::printf("Partitioned matching pipeline benchmark\n");
    std::printf("Orders: %zu across 8 symbols; queue capacity per shard: %zu\n",
                inputs.size(), args.queue_capacity);
    std::printf("Percentiles are approximate upper bounds from a power-of-two histogram.\n\n");
    std::printf("%-10s %-8s %-12s %-15s %-12s %-12s %-12s %-10s\n",
                "Producers", "Shards", "Elapsed(s)", "Commands/sec", "p50(ns)",
                "p99(ns)", "p99.9(ns)", "Q-high");

    const struct Configuration {
        size_t producers;
        size_t shards;
    } configurations[] = {{1, 1}, {2, 2}, {4, 4}, {8, 8}};

    bool all_valid = true;
    for (const Configuration config : configurations) {
        const RunResult result =
            runOnce(inputs, config.producers, config.shards, args.queue_capacity);
        const double throughput =
            result.elapsed_seconds > 0.0
                ? static_cast<double>(result.stats.processed) / result.elapsed_seconds
                : 0.0;
        std::printf("%-10zu %-8zu %-12.6f %-15.0f %-12llu %-12llu %-12llu %-10zu%s\n",
                    result.producers, result.shards, result.elapsed_seconds, throughput,
                    static_cast<unsigned long long>(result.stats.end_to_end_latency.p50_ns),
                    static_cast<unsigned long long>(result.stats.end_to_end_latency.p99_ns),
                    static_cast<unsigned long long>(result.stats.end_to_end_latency.p999_ns),
                    result.stats.max_queue_high_watermark,
                    result.valid ? "" : "  INVALID");
        all_valid = all_valid && result.valid;
    }

    std::printf("\nValidation: %s\n", all_valid ? "PASS" : "FAIL");
    return all_valid ? 0 : 2;
}
