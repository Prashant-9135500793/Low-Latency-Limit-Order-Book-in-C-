#include "matching_engine.hpp"
#include "test_common.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

using namespace lob;

namespace {

void checkNewOrderReport(const Order& input, const ExecutionReport& report,
                         const std::vector<Trade>& trades, uint64_t expected_sequence) {
    CHECK(report.command == CommandType::NEW_ORDER);
    CHECK(report.sequence == expected_sequence);
    CHECK(report.order_id == input.id);
    CHECK(report.requested_quantity == input.quantity);
    CHECK(report.trade_count == trades.size());

    uint64_t traded_quantity = 0;
    for (const Trade& trade : trades) {
        CHECK(trade.trade_id > 0);
        CHECK(trade.quantity > 0);
        CHECK(trade.price > 0);
        CHECK(trade.sequence == report.sequence);
        CHECK(trade.buy_order_id == input.id || trade.sell_order_id == input.id);
        traded_quantity += trade.quantity;
    }
    CHECK(traded_quantity == report.executed_quantity);
    CHECK(report.executed_quantity <= report.requested_quantity);
    CHECK(report.remaining_quantity ==
          report.requested_quantity - report.executed_quantity);

    if (report.status == ExecutionStatus::REJECTED) {
        CHECK(report.reject_reason != RejectReason::NONE);
        CHECK(trades.empty());
    } else {
        CHECK(report.reject_reason == RejectReason::NONE);
    }

    switch (report.status) {
    case ExecutionStatus::REJECTED:
        break;
    case ExecutionStatus::RESTING:
        CHECK(report.executed_quantity == 0);
        CHECK(report.remaining_quantity > 0);
        break;
    case ExecutionStatus::PARTIALLY_FILLED:
        CHECK(report.executed_quantity > 0);
        CHECK(report.remaining_quantity > 0);
        break;
    case ExecutionStatus::FILLED:
        CHECK(report.executed_quantity == report.requested_quantity);
        CHECK(report.remaining_quantity == 0);
        break;
    case ExecutionStatus::EXPIRED:
        CHECK(report.remaining_quantity > 0);
        break;
    case ExecutionStatus::CANCELED:
        CHECK(false && "new order cannot return CANCELED");
        break;
    }
}

void test_randomized_command_stream_preserves_invariants() {
    constexpr size_t kCommands = 75'000;
    std::mt19937_64 rng(0xC0FFEE123456789ULL);
    std::uniform_int_distribution<int> command_dist(0, 99);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int64_t> price_dist(9'900, 10'100);
    std::uniform_int_distribution<uint32_t> quantity_dist(1, 500);
    std::uniform_int_distribution<int> policy_dist(0, 99);

    MatchingEngine engine;
    std::vector<uint64_t> issued_ids;
    issued_ids.reserve(kCommands);
    uint64_t next_id = 1;
    uint64_t expected_sequence = 0;

    for (size_t command_index = 0; command_index < kCommands; ++command_index) {
        const int command_choice = command_dist(rng);
        if (command_choice < 12 && !issued_ids.empty()) {
            std::uniform_int_distribution<size_t> id_dist(0, issued_ids.size() - 1);
            const uint64_t id = issued_ids[id_dist(rng)];
            const ExecutionReport report = engine.cancel(id);
            ++expected_sequence;
            CHECK(report.sequence == expected_sequence);
            CHECK(report.command == CommandType::CANCEL_ORDER);
            CHECK(report.order_id == id);
            CHECK(report.status == ExecutionStatus::CANCELED ||
                  report.status == ExecutionStatus::REJECTED);
            if (report.status == ExecutionStatus::REJECTED) {
                CHECK(report.reject_reason == RejectReason::ORDER_NOT_FOUND);
            } else {
                CHECK(report.reject_reason == RejectReason::NONE);
            }
        } else {
            Order order;
            // Reuse a previous id sometimes to exercise lifetime duplicate-id rejection.
            if (command_choice < 18 && !issued_ids.empty()) {
                std::uniform_int_distribution<size_t> id_dist(0, issued_ids.size() - 1);
                order.id = issued_ids[id_dist(rng)];
            } else {
                order.id = next_id++;
                issued_ids.push_back(order.id);
            }
            order.side = side_dist(rng) == 0 ? Side::BUY : Side::SELL;
            order.type = OrderType::LIMIT;
            order.time_in_force = TimeInForce::GTC;
            order.price = price_dist(rng);
            order.quantity = quantity_dist(rng);

            const int policy = policy_dist(rng);
            if (policy < 6) {
                order.type = OrderType::MARKET;
                order.time_in_force = TimeInForce::IOC;
                order.price = 0;
            } else if (policy < 18) {
                order.time_in_force = TimeInForce::IOC;
            } else if (policy < 27) {
                order.time_in_force = TimeInForce::FOK;
            } else if (policy < 34) {
                order.flags = OrderFlags::POST_ONLY;
            } else if (policy == 99) {
                // Deliberately malformed but still pointer-free/well-defined.
                order.quantity = 0;
            }

            std::vector<Trade> trades;
            const ExecutionReport report = engine.processOrder(order, trades);
            ++expected_sequence;
            checkNewOrderReport(order, report, trades, expected_sequence);
        }

        if ((command_index & 0x7Fu) == 0) {
            CHECK(engine.book().checkInvariants());
            const auto bid = engine.book().bestBid();
            const auto ask = engine.book().bestAsk();
            CHECK(!bid || !ask || *bid < *ask);
        }
    }

    CHECK(engine.sequence() == expected_sequence);
    CHECK(engine.book().checkInvariants());
    CHECK(engine.acceptedOrderCount() + engine.rejectedOrderCount() <= kCommands);
}

void test_randomized_risk_limits_and_kill_switch() {
    RiskLimits limits;
    limits.max_order_quantity = 100;
    limits.min_limit_price = 9'950;
    limits.max_limit_price = 10'050;
    limits.max_order_notional = 1'000'000;
    limits.max_active_orders = 64;
    MatchingEngine engine(limits);

    std::mt19937_64 rng(77);
    std::uniform_int_distribution<int64_t> price_dist(9'900, 10'100);
    std::uniform_int_distribution<uint32_t> quantity_dist(1, 150);
    std::vector<Trade> trades;

    for (uint64_t id = 1; id <= 5'000; ++id) {
        Order order;
        order.id = id;
        order.side = (id & 1U) == 0 ? Side::BUY : Side::SELL;
        order.price = price_dist(rng);
        order.quantity = quantity_dist(rng);
        trades.clear();
        const ExecutionReport report = engine.processOrder(order, trades);
        CHECK(report.sequence == id);
        CHECK(report.status != ExecutionStatus::CANCELED);
        CHECK(engine.book().orderCount() <= limits.max_active_orders);
        if ((id % 97) == 0) CHECK(engine.book().checkInvariants());
    }

    engine.riskManager().setKillSwitch(true);
    Order blocked;
    blocked.id = 6'000;
    blocked.side = Side::BUY;
    blocked.price = 10'000;
    blocked.quantity = 1;
    trades.clear();
    const ExecutionReport report = engine.processOrder(blocked, trades);
    CHECK(report.status == ExecutionStatus::REJECTED);
    CHECK(report.reject_reason == RejectReason::RISK_KILL_SWITCH);
    CHECK(trades.empty());
    CHECK(engine.book().checkInvariants());
}

} // namespace

int main() {
    RUN(test_randomized_command_stream_preserves_invariants);
    RUN(test_randomized_risk_limits_and_kill_switch);
    return test_summary();
}
