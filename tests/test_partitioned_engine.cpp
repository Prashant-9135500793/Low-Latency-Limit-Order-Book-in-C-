#include "partitioned_engine.hpp"
#include "test_common.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace lob;

namespace {

Order makeOrder(uint64_t id, Side side, int64_t price, uint32_t quantity,
                OrderType type = OrderType::LIMIT,
                TimeInForce tif = TimeInForce::GTC) {
    Order order;
    order.id = id;
    order.side = side;
    order.type = type;
    order.time_in_force = tif;
    order.price = price;
    order.quantity = quantity;
    return order;
}

} // namespace

void test_partitioned_engine_preserves_fifo_per_shard() {
    PartitionedMatchingEngine engine(2, 8);
    const Symbol symbol("AAPL");

    CHECK(engine.submit(symbol, makeOrder(1, Side::SELL, 100, 10)).get().report.accepted());
    CHECK(engine.submit(symbol, makeOrder(2, Side::SELL, 100, 10)).get().report.accepted());

    const auto aggressive =
        engine.submit(symbol, makeOrder(3, Side::BUY, 100, 15)).get();
    CHECK(aggressive.report.status == ExecutionStatus::FILLED);
    CHECK(aggressive.trades.size() == 2);
    CHECK(aggressive.trades[0].sell_order_id == 1);
    CHECK(aggressive.trades[0].quantity == 10);
    CHECK(aggressive.trades[1].sell_order_id == 2);
    CHECK(aggressive.trades[1].quantity == 5);

    const auto snapshot = engine.snapshot(symbol);
    CHECK(snapshot.exists);
    CHECK(snapshot.order_count == 1);
    CHECK(snapshot.best_ask.value() == 100);
    CHECK(engine.validate(symbol));
}

void test_partitioned_engine_supports_advanced_orders() {
    PartitionedMatchingEngine engine(2, 16);
    const Symbol symbol("MSFT");
    CHECK(engine.submit(symbol, makeOrder(1, Side::SELL, 100, 10)).get().report.accepted());

    auto market = makeOrder(2, Side::BUY, 0, 20, OrderType::MARKET, TimeInForce::IOC);
    const auto result = engine.submit(symbol, market).get();
    CHECK(result.report.status == ExecutionStatus::PARTIALLY_FILLED);
    CHECK(result.report.executed_quantity == 10);
    CHECK(result.report.remaining_quantity == 10);
    CHECK(result.trades.size() == 1);
    CHECK(engine.snapshot(symbol).order_count == 0);
}

void test_partitioned_engine_many_producers_no_loss() {
    constexpr size_t kProducers = 4;
    constexpr size_t kPerProducer = 3000;
    constexpr size_t kTotal = kProducers * kPerProducer;
    const char* symbols[] = {"AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA", "META", "ORCL"};
    constexpr size_t kSymbolCount = sizeof(symbols) / sizeof(symbols[0]);

    PartitionedMatchingEngine engine(4, 64);
    std::atomic<uint64_t> next_id{1};
    std::atomic<bool> failed{false};
    std::vector<std::thread> producers;

    for (size_t producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (size_t i = 0; i < kPerProducer; ++i) {
                Order order;
                order.id = next_id.fetch_add(1, std::memory_order_relaxed);
                order.side = ((i + producer) & 1u) == 0 ? Side::BUY : Side::SELL;
                order.price = 10000 + static_cast<int64_t>((i * 7 + producer) % 21) - 10;
                order.quantity = 1 + static_cast<uint32_t>(i % 20);
                if (!engine.enqueue(Symbol(symbols[(i + producer) % kSymbolCount]), order)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    for (auto& producer : producers) producer.join();
    engine.waitUntilIdle();

    const auto stats = engine.stats();
    CHECK(!failed.load());
    CHECK(stats.submitted == kTotal);
    CHECK(stats.processed == kTotal);
    CHECK(stats.accepted + stats.rejected == kTotal);
    CHECK(stats.pending == 0);
    CHECK(stats.end_to_end_latency.count == kTotal);
    CHECK(stats.max_queue_high_watermark > 0);
    CHECK(stats.max_queue_high_watermark <= 64);

    for (const char* symbol : symbols) {
        CHECK(engine.validate(Symbol(symbol)));
    }
}

void test_partitioned_engine_shutdown_rejects_new_work() {
    PartitionedMatchingEngine engine(2, 4);
    engine.shutdown();
    const auto result = engine.submit(Symbol("AAPL"), makeOrder(1, Side::BUY, 100, 1)).get();
    CHECK(result.report.status == ExecutionStatus::REJECTED);
    CHECK(result.report.reject_reason == RejectReason::ENGINE_SHUTTING_DOWN);
}

void test_partitioned_engine_validates_configuration() {
    bool threw = false;
    try {
        PartitionedMatchingEngine invalid(3, 8);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        PartitionedMatchingEngine invalid(2, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    RUN(test_partitioned_engine_preserves_fifo_per_shard);
    RUN(test_partitioned_engine_supports_advanced_orders);
    RUN(test_partitioned_engine_many_producers_no_loss);
    RUN(test_partitioned_engine_shutdown_rejects_new_work);
    RUN(test_partitioned_engine_validates_configuration);
    return test_summary();
}
