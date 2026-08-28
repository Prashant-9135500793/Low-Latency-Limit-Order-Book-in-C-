#include "order_book.hpp"
#include "test_common.hpp"

using namespace lob;

static Order mk(uint64_t id, Side side, int64_t price, uint32_t qty, uint64_t seq) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = OrderType::LIMIT;
    o.price = price;
    o.quantity = qty;
    o.sequence = seq;
    return o;
}

void test_empty_book() {
    OrderBook book;
    CHECK(book.empty());
    CHECK(!book.bestBid().has_value());
    CHECK(!book.bestAsk().has_value());
    CHECK(!book.spread().has_value());
    CHECK(book.depth(Side::BUY) == 0);
    CHECK(book.depth(Side::SELL) == 0);
    CHECK(book.checkInvariants());
}

void test_add_buy() {
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 10, 1));
    CHECK(book.bestBid().value() == 100);
    CHECK(!book.bestAsk().has_value());
    CHECK(book.depth(Side::BUY) == 1);
    CHECK(book.orderCount() == 1);
    CHECK(book.checkInvariants());
}

void test_add_sell() {
    OrderBook book;
    book.addOrder(mk(1, Side::SELL, 105, 10, 1));
    CHECK(book.bestAsk().value() == 105);
    CHECK(!book.bestBid().has_value());
    CHECK(book.checkInvariants());
}

void test_best_bid_ask_multi_level() {
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 10, 1));
    book.addOrder(mk(2, Side::BUY, 105, 10, 2)); // higher price -> new best bid
    book.addOrder(mk(3, Side::BUY, 95, 10, 3));
    CHECK(book.bestBid().value() == 105);
    CHECK(book.depth(Side::BUY) == 3);

    book.addOrder(mk(4, Side::SELL, 110, 10, 4));
    book.addOrder(mk(5, Side::SELL, 108, 10, 5)); // lower price -> new best ask
    CHECK(book.bestAsk().value() == 108);
    CHECK(book.checkInvariants());
}

void test_spread() {
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 10, 1));
    book.addOrder(mk(2, Side::SELL, 103, 10, 2));
    CHECK(book.spread().value() == 3);
}

void test_price_priority_buy() {
    // At the BUY side, higher price must be the front (best) level.
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 10, 1));
    book.addOrder(mk(2, Side::BUY, 102, 10, 2));
    book.addOrder(mk(3, Side::BUY, 101, 10, 3));
    CHECK(book.frontOfBest(Side::BUY)->price == 102);
}

void test_price_priority_sell() {
    OrderBook book;
    book.addOrder(mk(1, Side::SELL, 105, 10, 1));
    book.addOrder(mk(2, Side::SELL, 103, 10, 2));
    book.addOrder(mk(3, Side::SELL, 104, 10, 3));
    CHECK(book.frontOfBest(Side::SELL)->price == 103);
}

void test_fifo_priority_same_price() {
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 50, 1));
    book.addOrder(mk(2, Side::BUY, 100, 70, 2));
    book.addOrder(mk(3, Side::BUY, 100, 30, 3));

    // Front must be order 1 (earliest sequence), and after reducing it
    // to zero the front should become order 2, then order 3.
    CHECK(book.frontOfBest(Side::BUY)->id == 1);
    book.reduceFront(Side::BUY, 50);
    CHECK(book.frontOfBest(Side::BUY)->id == 2);
    book.reduceFront(Side::BUY, 70);
    CHECK(book.frontOfBest(Side::BUY)->id == 3);
    book.reduceFront(Side::BUY, 30);
    CHECK(book.emptySide(Side::BUY));
}

void test_multiple_price_levels_depth() {
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 10, 1));
    book.addOrder(mk(2, Side::BUY, 101, 10, 2));
    book.addOrder(mk(3, Side::BUY, 100, 5, 3)); // same level as order 1
    CHECK(book.depth(Side::BUY) == 2);
    CHECK(book.orderCount() == 3);
}

void test_cancellation() {
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 10, 1));
    book.addOrder(mk(2, Side::BUY, 100, 20, 2));
    CHECK(book.cancelOrder(1));
    CHECK(book.frontOfBest(Side::BUY)->id == 2);
    CHECK(!book.cancelOrder(1)); // already gone
    CHECK(book.cancelOrder(2));
    CHECK(book.emptySide(Side::BUY));
    CHECK(!book.cancelOrder(999)); // never existed
}

void test_cancel_removes_empty_level() {
    OrderBook book;
    book.addOrder(mk(1, Side::BUY, 100, 10, 1));
    CHECK(book.depth(Side::BUY) == 1);
    book.cancelOrder(1);
    CHECK(book.depth(Side::BUY) == 0);
}

void test_reduce_front_partial_keeps_order() {
    OrderBook book;
    book.addOrder(mk(1, Side::SELL, 100, 100, 1));
    book.reduceFront(Side::SELL, 40);
    const Order* front = book.frontOfBest(Side::SELL);
    CHECK(front != nullptr);
    CHECK(front->id == 1);
    CHECK(front->quantity == 60);
    CHECK(book.orderCount() == 1);
}

void test_executable_quantity_and_depth_snapshot() {
    OrderBook book;
    book.addOrder(mk(1, Side::SELL, 100, 10, 1));
    book.addOrder(mk(2, Side::SELL, 100, 15, 2));
    book.addOrder(mk(3, Side::SELL, 101, 20, 3));

    Order incoming = mk(9, Side::BUY, 100, 100, 9);
    CHECK(book.executableQuantity(incoming) == 25);
    incoming.type = OrderType::MARKET;
    CHECK(book.executableQuantity(incoming) == 45);
    CHECK(book.executableQuantity(incoming, 30) == 30);

    const auto levels = book.topLevels(Side::SELL, 2);
    CHECK(levels.size() == 2);
    CHECK(levels[0].price == 100);
    CHECK(levels[0].total_quantity == 25);
    CHECK(levels[0].order_count == 2);
    CHECK(levels[1].price == 101);
}

void test_duplicate_active_id_is_refused() {
    OrderBook book;
    CHECK(book.addOrder(mk(1, Side::BUY, 100, 10, 1)));
    CHECK(!book.addOrder(mk(1, Side::BUY, 99, 10, 2)));
    CHECK(book.orderCount() == 1);
    CHECK(book.checkInvariants());
}

int main() {
    RUN(test_empty_book);
    RUN(test_add_buy);
    RUN(test_add_sell);
    RUN(test_best_bid_ask_multi_level);
    RUN(test_spread);
    RUN(test_price_priority_buy);
    RUN(test_price_priority_sell);
    RUN(test_fifo_priority_same_price);
    RUN(test_multiple_price_levels_depth);
    RUN(test_cancellation);
    RUN(test_cancel_removes_empty_level);
    RUN(test_reduce_front_partial_keeps_order);
    RUN(test_executable_quantity_and_depth_snapshot);
    RUN(test_duplicate_active_id_is_refused);
    return test_summary();
}
