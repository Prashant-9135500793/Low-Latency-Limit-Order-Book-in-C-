// benchmark_order_book.cpp
//
// Measures MatchingEngine::submitOrder() throughput and per-call
// latency for a deterministic synthetic workload, at several order
// counts. Workload mix is configurable (see docs/performance.md,
// Experiment D): a fraction of orders are generated to cross the
// book (causing a match) and the rest to rest passively.

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "benchmark.hpp"
#include "matching_engine.hpp"
#include "timestamp.hpp"

using namespace lob;

namespace {

struct Workload {
    std::string name;
    double crossing_fraction; // probability an order is generated to cross
};

// Generates the i-th order for a given workload. Deterministic given
// (workload, seed, i).
Order makeOrder(std::mt19937_64& rng, uint64_t id, double crossing_fraction,
                 int64_t& running_mid) {
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 500);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::uniform_int_distribution<int64_t> offset_dist(1, 50);

    Order o;
    o.id = id;
    o.type = OrderType::LIMIT;
    Side side = side_dist(rng) == 0 ? Side::BUY : Side::SELL;
    o.side = side;
    o.quantity = qty_dist(rng);

    const bool cross = coin(rng) < crossing_fraction;
    const int64_t offset = offset_dist(rng);
    if (side == Side::BUY) {
        o.price = cross ? running_mid + offset : running_mid - offset;
    } else {
        o.price = cross ? running_mid - offset : running_mid + offset;
    }
    // Slowly random-walk the reference mid so prices don't stay static.
    if (id % 97 == 0) {
        running_mid += (side_dist(rng) == 0 ? 1 : -1);
    }
    return o;
}

void runWorkload(const Workload& wl, uint64_t n) {
    std::printf("\n=== Order book benchmark: %s, n=%llu ===\n", wl.name.c_str(),
                static_cast<unsigned long long>(n));

    std::mt19937_64 rng(12345);
    int64_t mid = 10000;

    // Pre-generate all orders so generation cost is not included in
    // the timed region.
    std::vector<Order> orders;
    orders.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
        orders.push_back(makeOrder(rng, i + 1, wl.crossing_fraction, mid));
    }

    busy_warmup(1'000'000);

    MatchingEngine engine;
    std::vector<Trade> trades;
    trades.reserve(8);
    std::vector<uint64_t> latencies;
    latencies.reserve(n);

    const uint64_t t_start = now_ns();
    for (const auto& base_order : orders) {
        trades.clear();
        const uint64_t t0 = now_ns();
        engine.submitOrder(base_order, trades);
        const uint64_t t1 = now_ns();
        latencies.push_back(t1 - t0);
    }
    const uint64_t t_end = now_ns();

    const double secs = static_cast<double>(t_end - t_start) / 1e9;
    bench::printThroughput("order_book " + wl.name, n, secs);

    auto stats = bench::computeStats(latencies);
    bench::printStats("order_book " + wl.name + " per-order latency", stats);

    std::printf("  trades generated: %llu\n",
                static_cast<unsigned long long>(engine.tradeCount()));
    std::printf("  final depth: BUY=%zu SELL=%zu\n", engine.book().depth(Side::BUY),
                engine.book().depth(Side::SELL));
}

} // namespace

int main() {
    const std::vector<Workload> workloads = {
        {"mostly_non_crossing", 0.05},
        {"mixed", 0.50},
        {"mostly_crossing", 0.95},
    };
    const std::vector<uint64_t> sizes = {100'000, 1'000'000, 5'000'000};

    for (const auto& wl : workloads) {
        for (uint64_t n : sizes) {
            runWorkload(wl, n);
        }
    }
    return 0;
}
