#include "order_book.hpp"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace lob {

bool OrderBook::addOrder(const Order& order) {
    if (order.id == 0 || order.quantity == 0 || order.price <= 0 ||
        order.type != OrderType::LIMIT || contains(order.id)) {
        return false;
    }

    if (order.side == Side::BUY) {
        auto [level_it, inserted_level] = buy_levels_.try_emplace(order.price);
        (void)inserted_level;
        level_it->second.orders.push_back(order);
        level_it->second.total_quantity += order.quantity;
        auto order_it = std::prev(level_it->second.orders.end());
        const auto [_, inserted] =
            locations_.emplace(order.id, OrderLocation{Side::BUY, order.price, order_it});
        assert(inserted);
        (void)inserted;
    } else if (order.side == Side::SELL) {
        auto [level_it, inserted_level] = sell_levels_.try_emplace(order.price);
        (void)inserted_level;
        level_it->second.orders.push_back(order);
        level_it->second.total_quantity += order.quantity;
        auto order_it = std::prev(level_it->second.orders.end());
        const auto [_, inserted] =
            locations_.emplace(order.id, OrderLocation{Side::SELL, order.price, order_it});
        assert(inserted);
        (void)inserted;
    } else {
        return false;
    }
    return true;
}

bool OrderBook::cancelOrder(uint64_t order_id, Order* canceled_order) {
    auto loc_it = locations_.find(order_id);
    if (loc_it == locations_.end()) return false;

    const OrderLocation loc = loc_it->second;
    if (canceled_order != nullptr) *canceled_order = *loc.order_it;
    locations_.erase(loc_it);

    if (loc.side == Side::BUY) {
        auto level_it = buy_levels_.find(loc.price);
        assert(level_it != buy_levels_.end());
        level_it->second.total_quantity -= loc.order_it->quantity;
        level_it->second.orders.erase(loc.order_it);
        eraseLevelIfEmpty(buy_levels_, level_it);
    } else {
        auto level_it = sell_levels_.find(loc.price);
        assert(level_it != sell_levels_.end());
        level_it->second.total_quantity -= loc.order_it->quantity;
        level_it->second.orders.erase(loc.order_it);
        eraseLevelIfEmpty(sell_levels_, level_it);
    }
    return true;
}

std::optional<int64_t> OrderBook::bestBid() const {
    if (buy_levels_.empty()) return std::nullopt;
    return buy_levels_.begin()->first;
}

std::optional<int64_t> OrderBook::bestAsk() const {
    if (sell_levels_.empty()) return std::nullopt;
    return sell_levels_.begin()->first;
}

std::optional<int64_t> OrderBook::spread() const {
    const auto bid = bestBid();
    const auto ask = bestAsk();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

size_t OrderBook::depth(Side side) const {
    return side == Side::BUY ? buy_levels_.size() : sell_levels_.size();
}

bool OrderBook::emptySide(Side side) const {
    return side == Side::BUY ? buy_levels_.empty() : sell_levels_.empty();
}

const Order* OrderBook::frontOfBest(Side side) const {
    if (side == Side::BUY) {
        if (buy_levels_.empty()) return nullptr;
        return &buy_levels_.begin()->second.orders.front();
    }
    if (sell_levels_.empty()) return nullptr;
    return &sell_levels_.begin()->second.orders.front();
}

void OrderBook::reduceFront(Side side, uint32_t qty) {
    if (side == Side::BUY) {
        assert(!buy_levels_.empty());
        auto level_it = buy_levels_.begin();
        auto order_it = level_it->second.orders.begin();
        assert(qty <= order_it->quantity);
        order_it->quantity -= qty;
        level_it->second.total_quantity -= qty;
        if (order_it->quantity == 0) {
            locations_.erase(order_it->id);
            level_it->second.orders.erase(order_it);
            eraseLevelIfEmpty(buy_levels_, level_it);
        }
    } else {
        assert(!sell_levels_.empty());
        auto level_it = sell_levels_.begin();
        auto order_it = level_it->second.orders.begin();
        assert(qty <= order_it->quantity);
        order_it->quantity -= qty;
        level_it->second.total_quantity -= qty;
        if (order_it->quantity == 0) {
            locations_.erase(order_it->id);
            level_it->second.orders.erase(order_it);
            eraseLevelIfEmpty(sell_levels_, level_it);
        }
    }
}

bool OrderBook::wouldCross(const Order& incoming) const {
    if (!isValidSide(incoming.side) || !isValidOrderType(incoming.type)) return false;
    const Side opposite = incoming.side == Side::BUY ? Side::SELL : Side::BUY;
    const Order* best = frontOfBest(opposite);
    if (best == nullptr) return false;
    if (incoming.type == OrderType::MARKET) return true;
    return incoming.side == Side::BUY ? incoming.price >= best->price
                                      : incoming.price <= best->price;
}

uint64_t OrderBook::executableQuantity(const Order& incoming, uint64_t cap) const {
    if (cap == 0 || !isValidSide(incoming.side) || !isValidOrderType(incoming.type)) {
        return 0;
    }

    uint64_t total = 0;
    const auto add_level = [&](const PriceLevel& level) {
        const uint64_t remaining_cap = cap - total;
        total += std::min(level.total_quantity, remaining_cap);
    };

    if (incoming.side == Side::BUY) {
        for (const auto& [price, level] : sell_levels_) {
            if (incoming.type == OrderType::LIMIT && incoming.price < price) break;
            add_level(level);
            if (total == cap) break;
        }
    } else {
        for (const auto& [price, level] : buy_levels_) {
            if (incoming.type == OrderType::LIMIT && incoming.price > price) break;
            add_level(level);
            if (total == cap) break;
        }
    }
    return total;
}

std::vector<LevelSnapshot> OrderBook::topLevels(Side side, size_t max_levels) const {
    std::vector<LevelSnapshot> out;
    out.reserve(std::min(max_levels, depth(side)));

    if (side == Side::BUY) {
        for (const auto& [price, level] : buy_levels_) {
            if (out.size() == max_levels) break;
            out.push_back(LevelSnapshot{price, level.total_quantity, level.orders.size()});
        }
    } else {
        for (const auto& [price, level] : sell_levels_) {
            if (out.size() == max_levels) break;
            out.push_back(LevelSnapshot{price, level.total_quantity, level.orders.size()});
        }
    }
    return out;
}

template <typename MapT>
void OrderBook::eraseLevelIfEmpty(MapT& levels, typename MapT::iterator it) {
    if (it->second.orders.empty()) levels.erase(it);
}

bool OrderBook::checkInvariants() const {
    if (const auto s = spread(); s && *s <= 0) return false;

    uint64_t seen_orders = 0;
    std::unordered_set<uint64_t> seen_ids;
    seen_ids.reserve(locations_.size());

    const auto check_level = [&](Side expected_side, int64_t price,
                                 const PriceLevel& level) -> bool {
        if (level.orders.empty()) return false;
        uint64_t sum = 0;
        uint64_t previous_sequence = 0;
        bool first = true;
        for (const auto& order : level.orders) {
            if (order.id == 0 || order.quantity == 0 || order.price != price ||
                order.side != expected_side || order.type != OrderType::LIMIT) {
                return false;
            }
            if (!first && order.sequence < previous_sequence) return false;
            first = false;
            previous_sequence = order.sequence;
            sum += order.quantity;
            ++seen_orders;
            if (!seen_ids.insert(order.id).second) return false;

            const auto loc = locations_.find(order.id);
            if (loc == locations_.end() || loc->second.side != expected_side ||
                loc->second.price != price || &*loc->second.order_it != &order) {
                return false;
            }
        }
        return sum == level.total_quantity;
    };

    int64_t previous_price = 0;
    bool first_level = true;
    for (const auto& [price, level] : buy_levels_) {
        if (!first_level && price >= previous_price) return false;
        first_level = false;
        previous_price = price;
        if (!check_level(Side::BUY, price, level)) return false;
    }

    first_level = true;
    for (const auto& [price, level] : sell_levels_) {
        if (!first_level && price <= previous_price) return false;
        first_level = false;
        previous_price = price;
        if (!check_level(Side::SELL, price, level)) return false;
    }

    return seen_orders == locations_.size();
}

} // namespace lob
