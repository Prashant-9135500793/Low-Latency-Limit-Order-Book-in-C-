#pragma once
// execution_report.hpp
//
// Explicit command outcome returned by MatchingEngine and optionally
// published over shared memory. A professional order-entry path needs
// more than a trade count: callers must know whether an order rested,
// filled, expired, was canceled, or was rejected and why.

#include <cstdint>
#include <type_traits>

namespace lob {

enum class CommandType : uint8_t {
    NEW_ORDER = 0,
    CANCEL_ORDER = 1,
};

enum class ExecutionStatus : uint8_t {
    REJECTED = 0,
    RESTING = 1,
    PARTIALLY_FILLED = 2,
    FILLED = 3,
    EXPIRED = 4,
    CANCELED = 5,
};

enum class RejectReason : uint8_t {
    NONE = 0,
    INVALID_ORDER_ID,
    DUPLICATE_ORDER_ID,
    INVALID_SIDE,
    INVALID_ORDER_TYPE,
    INVALID_TIME_IN_FORCE,
    INVALID_ORDER_FLAGS,
    INVALID_QUANTITY,
    INVALID_LIMIT_PRICE,
    MARKET_ORDER_CANNOT_BE_GTC,
    POST_ONLY_REQUIRES_GTC_LIMIT,
    POST_ONLY_WOULD_TRADE,
    FOK_NOT_FILLABLE,
    RISK_KILL_SWITCH,
    RISK_MAX_ORDER_QUANTITY,
    RISK_PRICE_OUT_OF_RANGE,
    RISK_MAX_NOTIONAL,
    RISK_MAX_ACTIVE_ORDERS,
    ORDER_NOT_FOUND,
    INGRESS_QUEUE_FULL,
    ENGINE_SHUTTING_DOWN,
    JOURNAL_ERROR,
};

struct ExecutionReport {
    uint64_t order_id = 0;
    uint64_t sequence = 0;
    CommandType command = CommandType::NEW_ORDER;
    ExecutionStatus status = ExecutionStatus::REJECTED;
    RejectReason reject_reason = RejectReason::NONE;
    uint8_t reserved = 0;
    uint32_t requested_quantity = 0;
    uint32_t executed_quantity = 0;
    uint32_t remaining_quantity = 0;
    uint32_t trade_count = 0;

    constexpr bool accepted() const noexcept {
        return status != ExecutionStatus::REJECTED;
    }
};

static_assert(std::is_trivially_copyable_v<ExecutionReport>);
static_assert(std::is_standard_layout_v<ExecutionReport>);

const char* toString(ExecutionStatus status) noexcept;
const char* toString(RejectReason reason) noexcept;
const char* toString(CommandType command) noexcept;

} // namespace lob
