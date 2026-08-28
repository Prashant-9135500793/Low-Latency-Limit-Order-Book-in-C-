#include "matching_engine.hpp"
#include "test_common.hpp"

using namespace lob;

static Order mk(uint64_t id, Side side, int64_t price, uint32_t qty) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = OrderType::LIMIT;
    o.price = price;
    o.quantity = qty;
    return o;
}

void test_no_cross_rests_in_book() {
    MatchingEngine eng;
    std::vector<Trade> trades;
    size_t n = eng.submitOrder(mk(1, Side::BUY, 100, 10), trades);
    CHECK(n == 0);
    CHECK(trades.empty());
    CHECK(eng.book().bestBid().value() == 100);
}

void test_exact_match() {
    MatchingEngine eng;
    std::vector<Trade> trades;
    eng.submitOrder(mk(1, Side::SELL, 100, 50), trades);
    trades.clear();
    size_t n = eng.submitOrder(mk(2, Side::BUY, 100, 50), trades);
    CHECK(n == 1);
    CHECK(trades.size() == 1);
    CHECK(trades[0].price == 100);
    CHECK(trades[0].quantity == 50);
    CHECK(trades[0].buy_order_id == 2);
    CHECK(trades[0].sell_order_id == 1);
    CHECK(eng.book().empty()); // both fully filled
}

void test_partial_fill() {
    MatchingEngine eng;
    std::vector<Trade> trades;
    eng.submitOrder(mk(1, Side::SELL, 100, 30), trades);
    trades.clear();
    size_t n = eng.submitOrder(mk(2, Side::BUY, 100, 50), trades);
    CHECK(n == 1);
    CHECK(trades[0].quantity == 30);
    // 20 remaining quantity should now rest on the BUY side.
    CHECK(eng.book().bestBid().value() == 100);
    CHECK(eng.book().frontOfBest(Side::BUY)->quantity == 20);
    CHECK(eng.book().emptySide(Side::SELL));
}

void test_scenario_from_brief() {
    // SELL 100 @ 101
    // SELL 200 @ 102
    // BUY  150 @ 102
    // Expected: trade 100@101, trade 50@102, remaining SELL 150@102
    MatchingEngine eng;
    std::vector<Trade> trades;
    eng.submitOrder(mk(1, Side::SELL, 101, 100), trades);
    eng.submitOrder(mk(2, Side::SELL, 102, 200), trades);
    trades.clear();

    size_t n = eng.submitOrder(mk(3, Side::BUY, 102, 150), trades);
    CHECK(n == 2);
    CHECK(trades[0].price == 101);
    CHECK(trades[0].quantity == 100);
    CHECK(trades[0].sell_order_id == 1);
    CHECK(trades[1].price == 102);
    CHECK(trades[1].quantity == 50);
    CHECK(trades[1].sell_order_id == 2);

    CHECK(eng.book().emptySide(Side::BUY));
    CHECK(eng.book().bestAsk().value() == 102);
    CHECK(eng.book().frontOfBest(Side::SELL)->quantity == 150);
    CHECK(eng.book().frontOfBest(Side::SELL)->id == 2);
}

void test_fifo_matching_order() {
    // Two resting SELL orders at the same price; incoming BUY should
    // consume them in arrival order (order 1 before order 2).
    MatchingEngine eng;
    std::vector<Trade> trades;
    eng.submitOrder(mk(1, Side::SELL, 100, 40), trades);
    eng.submitOrder(mk(2, Side::SELL, 100, 40), trades);
    trades.clear();

    size_t n = eng.submitOrder(mk(3, Side::BUY, 100, 60), trades);
    CHECK(n == 2);
    CHECK(trades[0].sell_order_id == 1);
    CHECK(trades[0].quantity == 40);
    CHECK(trades[1].sell_order_id == 2);
    CHECK(trades[1].quantity == 20);
}

void test_sequence_monotonic() {
    MatchingEngine eng;
    std::vector<Trade> trades;
    eng.submitOrder(mk(1, Side::BUY, 100, 10), trades);
    uint64_t s1 = eng.sequence();
    eng.submitOrder(mk(2, Side::BUY, 101, 10), trades);
    uint64_t s2 = eng.sequence();
    CHECK(s2 > s1);
}

void test_cancel_via_engine() {
    MatchingEngine eng;
    std::vector<Trade> trades;
    eng.submitOrder(mk(1, Side::BUY, 100, 10), trades);
    CHECK(eng.cancelOrder(1));
    CHECK(eng.book().emptySide(Side::BUY));
    CHECK(!eng.cancelOrder(1));
}

void test_no_crossed_book_invariant_after_matching() {
    MatchingEngine eng;
    std::vector<Trade> trades;
    eng.submitOrder(mk(1, Side::SELL, 100, 1000), trades);
    for (uint64_t i = 2; i < 50; ++i) {
        eng.submitOrder(mk(i, Side::BUY, 90 + static_cast<int64_t>(i % 20), 10), trades);
    }
    CHECK(eng.book().checkInvariants());
}

int main() {
    RUN(test_no_cross_rests_in_book);
    RUN(test_exact_match);
    RUN(test_partial_fill);
    RUN(test_scenario_from_brief);
    RUN(test_fifo_matching_order);
    RUN(test_sequence_monotonic);
    RUN(test_cancel_via_engine);
    RUN(test_no_crossed_book_invariant_after_matching);
    return test_summary();
}
