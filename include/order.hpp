#pragma once
// order.hpp
//
// Fixed-size order representation shared by the in-process matching
// engines and the mmap IPC protocol. Prices are integer ticks rather
// than floating point so comparisons are exact and deterministic.

#include <cstdint>
#include <type_traits>

namespace lob {

enum class Side : uint8_t { BUY = 0, SELL = 1 };

enum class OrderType : uint8_t {
    LIMIT = 0,
    MARKET = 1,
};

// GTC: match immediately, then rest any unfilled LIMIT quantity.
// IOC: match immediately and expire any remainder.
// FOK: execute the full quantity immediately or reject without
//      changing the book.
enum class TimeInForce : uint8_t {
    GTC = 0,
    IOC = 1,
    FOK = 2,
};

enum class OrderFlags : uint8_t {
    NONE = 0,
    POST_ONLY = 1u << 0,
};

constexpr OrderFlags operator|(OrderFlags lhs, OrderFlags rhs) noexcept {
    return static_cast<OrderFlags>(static_cast<uint8_t>(lhs) |
                                   static_cast<uint8_t>(rhs));
}

constexpr bool hasFlag(OrderFlags value, OrderFlags flag) noexcept {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

constexpr bool isValidSide(Side side) noexcept {
    return side == Side::BUY || side == Side::SELL;
}

constexpr bool isValidOrderType(OrderType type) noexcept {
    return type == OrderType::LIMIT || type == OrderType::MARKET;
}

constexpr bool isValidTimeInForce(TimeInForce tif) noexcept {
    return tif == TimeInForce::GTC || tif == TimeInForce::IOC ||
           tif == TimeInForce::FOK;
}

// Trivially copyable and pointer-free: safe to place in shared memory,
// fixed-size queues, and the append-only binary journal.
struct Order {
    uint64_t id = 0;
    Side side = Side::BUY;
    OrderType type = OrderType::LIMIT;
    TimeInForce time_in_force = TimeInForce::GTC;
    OrderFlags flags = OrderFlags::NONE;
    int64_t price = 0;      // integer ticks; ignored for MARKET
    uint32_t quantity = 0;  // requested/remaining quantity
    uint64_t sequence = 0;  // assigned by the matching engine
};

static_assert(std::is_trivially_copyable_v<Order>,
              "Order must be trivially copyable to live in shared memory");
static_assert(std::is_standard_layout_v<Order>,
              "Order must have a stable, inspectable layout");

} // namespace lob
