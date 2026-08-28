#include "matching_engine.hpp"
#include "test_common.hpp"

#include <vector>

using namespace lob;

namespace {

Order makeOrder(uint64_t id, Side side, int64_t price, uint32_t quantity,
                OrderType type = OrderType::LIMIT,
                TimeInForce tif = TimeInForce::GTC,
                OrderFlags flags = OrderFlags::NONE) {
    Order order;
    order.id = id;
    order.side = side;
    order.type = type;
    order.time_in_force = tif;
    order.flags = flags;
    order.price = price;
    order.quantity = quantity;
    return order;
}

} // namespace

void test_market_ioc_sweeps_multiple_levels() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    engine.processOrder(makeOrder(1, Side::SELL, 100, 10), trades);
    engine.processOrder(makeOrder(2, Side::SELL, 101, 20), trades);
    trades.clear();

    const auto report = engine.processOrder(
        makeOrder(3, Side::BUY, 0, 25, OrderType::MARKET, TimeInForce::IOC), trades);

    CHECK(report.status == ExecutionStatus::FILLED);
    CHECK(report.executed_quantity == 25);
    CHECK(report.remaining_quantity == 0);
    CHECK(report.trade_count == 2);
    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 100);
    CHECK(trades[0].quantity == 10);
    CHECK(trades[1].price == 101);
    CHECK(trades[1].quantity == 15);
    CHECK(engine.book().frontOfBest(Side::SELL)->quantity == 5);
}

void test_ioc_partial_fill_never_rests_remainder() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    engine.processOrder(makeOrder(1, Side::SELL, 100, 10), trades);
    trades.clear();

    const auto report = engine.processOrder(
        makeOrder(2, Side::BUY, 100, 25, OrderType::LIMIT, TimeInForce::IOC), trades);

    CHECK(report.status == ExecutionStatus::PARTIALLY_FILLED);
    CHECK(report.executed_quantity == 10);
    CHECK(report.remaining_quantity == 15);
    CHECK(engine.book().empty());
}

void test_fok_rejects_without_mutating_book() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    engine.processOrder(makeOrder(1, Side::SELL, 100, 10), trades);
    const auto before_count = engine.book().orderCount();
    trades.clear();

    const auto report = engine.processOrder(
        makeOrder(2, Side::BUY, 100, 20, OrderType::LIMIT, TimeInForce::FOK), trades);

    CHECK(report.status == ExecutionStatus::REJECTED);
    CHECK(report.reject_reason == RejectReason::FOK_NOT_FILLABLE);
    CHECK(trades.empty());
    CHECK(engine.book().orderCount() == before_count);
    CHECK(engine.book().frontOfBest(Side::SELL)->quantity == 10);
}

void test_fok_full_fill() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    engine.processOrder(makeOrder(1, Side::SELL, 100, 10), trades);
    engine.processOrder(makeOrder(2, Side::SELL, 101, 10), trades);
    trades.clear();

    const auto report = engine.processOrder(
        makeOrder(3, Side::BUY, 101, 20, OrderType::LIMIT, TimeInForce::FOK), trades);

    CHECK(report.status == ExecutionStatus::FILLED);
    CHECK(report.trade_count == 2);
    CHECK(engine.book().empty());
}

void test_post_only_rejects_cross_and_rests_non_cross() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    engine.processOrder(makeOrder(1, Side::SELL, 100, 10), trades);
    trades.clear();

    const auto rejected = engine.processOrder(
        makeOrder(2, Side::BUY, 100, 5, OrderType::LIMIT, TimeInForce::GTC,
                  OrderFlags::POST_ONLY),
        trades);
    CHECK(rejected.status == ExecutionStatus::REJECTED);
    CHECK(rejected.reject_reason == RejectReason::POST_ONLY_WOULD_TRADE);
    CHECK(trades.empty());

    // Rejected ids are not consumed; the client can correct and retry.
    const auto accepted = engine.processOrder(
        makeOrder(2, Side::BUY, 99, 5, OrderType::LIMIT, TimeInForce::GTC,
                  OrderFlags::POST_ONLY),
        trades);
    CHECK(accepted.status == ExecutionStatus::RESTING);
    CHECK(engine.book().bestBid().value() == 99);
}

void test_market_gtc_is_rejected() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    const auto report = engine.processOrder(
        makeOrder(1, Side::BUY, 0, 10, OrderType::MARKET, TimeInForce::GTC), trades);
    CHECK(report.status == ExecutionStatus::REJECTED);
    CHECK(report.reject_reason == RejectReason::MARKET_ORDER_CANNOT_BE_GTC);
}

void test_duplicate_id_is_rejected_for_engine_lifetime() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    CHECK(engine.processOrder(makeOrder(1, Side::SELL, 100, 10), trades).accepted());
    trades.clear();
    CHECK(engine.processOrder(makeOrder(2, Side::BUY, 100, 10), trades).accepted());
    CHECK(engine.book().empty());

    const auto duplicate = engine.processOrder(makeOrder(2, Side::BUY, 99, 1), trades);
    CHECK(duplicate.status == ExecutionStatus::REJECTED);
    CHECK(duplicate.reject_reason == RejectReason::DUPLICATE_ORDER_ID);
}

void test_risk_limits_and_kill_switch() {
    RiskLimits limits;
    limits.max_order_quantity = 100;
    limits.min_limit_price = 90;
    limits.max_limit_price = 110;
    limits.max_order_notional = 1000;
    limits.max_active_orders = 1;
    MatchingEngine engine(limits);
    std::vector<Trade> trades;

    auto report = engine.processOrder(makeOrder(1, Side::BUY, 100, 101), trades);
    CHECK(report.reject_reason == RejectReason::RISK_MAX_ORDER_QUANTITY);

    report = engine.processOrder(makeOrder(2, Side::BUY, 111, 1), trades);
    CHECK(report.reject_reason == RejectReason::RISK_PRICE_OUT_OF_RANGE);

    report = engine.processOrder(makeOrder(3, Side::BUY, 100, 11), trades);
    CHECK(report.reject_reason == RejectReason::RISK_MAX_NOTIONAL);

    report = engine.processOrder(makeOrder(4, Side::SELL, 100, 10), trades);
    CHECK(report.status == ExecutionStatus::RESTING);

    report = engine.processOrder(makeOrder(5, Side::SELL, 101, 1), trades);
    CHECK(report.reject_reason == RejectReason::RISK_MAX_ACTIVE_ORDERS);

    // A fully crossing GTC order cannot increase active order count,
    // so it remains allowed at the active-order limit.
    report = engine.processOrder(makeOrder(6, Side::BUY, 100, 10), trades);
    CHECK(report.status == ExecutionStatus::FILLED);

    engine.riskManager().setKillSwitch(true);
    report = engine.processOrder(makeOrder(7, Side::BUY, 99, 1), trades);
    CHECK(report.reject_reason == RejectReason::RISK_KILL_SWITCH);

    engine.riskManager().setKillSwitch(false);
    report = engine.processOrder(makeOrder(8, Side::BUY, 99, 1), trades);
    CHECK(report.status == ExecutionStatus::RESTING);
    CHECK(engine.cancel(8).status == ExecutionStatus::CANCELED);
}

void test_execution_report_sequences_every_command() {
    MatchingEngine engine;
    std::vector<Trade> trades;
    const auto first = engine.processOrder(makeOrder(1, Side::BUY, 100, 1), trades);
    const auto invalid = engine.processOrder(makeOrder(0, Side::BUY, 100, 1), trades);
    const auto cancel = engine.cancel(1);
    CHECK(first.sequence == 1);
    CHECK(invalid.sequence == 2);
    CHECK(cancel.sequence == 3);
    CHECK(cancel.command == CommandType::CANCEL_ORDER);
    CHECK(cancel.status == ExecutionStatus::CANCELED);
    CHECK(cancel.requested_quantity == 1);
    CHECK(cancel.remaining_quantity == 0);
}

int main() {
    RUN(test_market_ioc_sweeps_multiple_levels);
    RUN(test_ioc_partial_fill_never_rests_remainder);
    RUN(test_fok_rejects_without_mutating_book);
    RUN(test_fok_full_fill);
    RUN(test_post_only_rejects_cross_and_rests_non_cross);
    RUN(test_market_gtc_is_rejected);
    RUN(test_duplicate_id_is_rejected_for_engine_lifetime);
    RUN(test_risk_limits_and_kill_switch);
    RUN(test_execution_report_sequences_every_command);
    return test_summary();
}
