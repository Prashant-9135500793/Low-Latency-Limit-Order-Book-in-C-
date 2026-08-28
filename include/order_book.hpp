#pragma once
// order_book.hpp
//
// Price-time-priority limit order book. BUY levels are ordered highest
// first, SELL levels lowest first, and each level is a FIFO list.

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "order.hpp"

namespace lob {

struct PriceLevel {
    std::list<Order> orders;
    uint64_t total_quantity = 0;
};

struct LevelSnapshot {
    int64_t price = 0;
    uint64_t total_quantity = 0;
    size_t order_count = 0;
};

class OrderBook {
public:
    // Adds an already-sequenced resting LIMIT order. Returns false for
    // malformed input or a duplicate active order id.
    bool addOrder(const Order& order);

    bool cancelOrder(uint64_t order_id, Order* canceled_order);
    bool cancelOrder(uint64_t order_id) { return cancelOrder(order_id, nullptr); }
    bool contains(uint64_t order_id) const noexcept {
        return locations_.find(order_id) != locations_.end();
    }

    std::optional<int64_t> bestBid() const;
    std::optional<int64_t> bestAsk() const;
    std::optional<int64_t> spread() const;

    size_t depth(Side side) const;
    bool empty() const { return buy_levels_.empty() && sell_levels_.empty(); }
    bool emptySide(Side side) const;
    size_t orderCount() const { return locations_.size(); }

    const Order* frontOfBest(Side side) const;
    void reduceFront(Side side, uint32_t qty);

    // True when an incoming order can execute at least one unit at the
    // current top of book. MARKET orders cross any available opposite
    // liquidity; LIMIT orders obey their price.
    bool wouldCross(const Order& incoming) const;

    // Total immediately executable quantity, capped at `cap`. Used to
    // preflight FOK orders without mutating the book.
    uint64_t executableQuantity(const Order& incoming,
                                uint64_t cap = UINT64_MAX) const;

    // Aggregated top N price levels, best first. This is the beginning
    // of a market-data/depth API rather than exposing internal maps.
    std::vector<LevelSnapshot> topLevels(Side side, size_t max_levels) const;

    bool checkInvariants() const;

private:
    struct OrderLocation {
        Side side;
        int64_t price;
        std::list<Order>::iterator order_it;
    };

    using BuyMap = std::map<int64_t, PriceLevel, std::greater<int64_t>>;
    using SellMap = std::map<int64_t, PriceLevel, std::less<int64_t>>;

    BuyMap buy_levels_;
    SellMap sell_levels_;
    std::unordered_map<uint64_t, OrderLocation> locations_;

    template <typename MapT>
    void eraseLevelIfEmpty(MapT& levels, typename MapT::iterator it);
};

} // namespace lob
