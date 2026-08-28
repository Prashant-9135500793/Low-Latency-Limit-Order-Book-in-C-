#pragma once
// trade.hpp
// Result of a successful match between a resting order and an incoming order.

#include <cstdint>
#include <type_traits>

namespace lob {

struct Trade {
    uint64_t trade_id = 0;
    uint64_t buy_order_id = 0;
    uint64_t sell_order_id = 0;
    int64_t price = 0;      // execution price (the resting order's price)
    uint32_t quantity = 0;  // executed quantity
    uint64_t sequence = 0;  // global sequence at time of trade
};

static_assert(std::is_trivially_copyable_v<Trade>,
              "Trade must be trivially copyable to live in shared memory");

} // namespace lob
