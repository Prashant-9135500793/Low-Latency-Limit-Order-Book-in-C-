#include "execution_report.hpp"

namespace lob {

const char* toString(ExecutionStatus status) noexcept {
    switch (status) {
    case ExecutionStatus::REJECTED: return "REJECTED";
    case ExecutionStatus::RESTING: return "RESTING";
    case ExecutionStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
    case ExecutionStatus::FILLED: return "FILLED";
    case ExecutionStatus::EXPIRED: return "EXPIRED";
    case ExecutionStatus::CANCELED: return "CANCELED";
    }
    return "UNKNOWN_STATUS";
}

const char* toString(RejectReason reason) noexcept {
    switch (reason) {
    case RejectReason::NONE: return "NONE";
    case RejectReason::INVALID_ORDER_ID: return "INVALID_ORDER_ID";
    case RejectReason::DUPLICATE_ORDER_ID: return "DUPLICATE_ORDER_ID";
    case RejectReason::INVALID_SIDE: return "INVALID_SIDE";
    case RejectReason::INVALID_ORDER_TYPE: return "INVALID_ORDER_TYPE";
    case RejectReason::INVALID_TIME_IN_FORCE: return "INVALID_TIME_IN_FORCE";
    case RejectReason::INVALID_ORDER_FLAGS: return "INVALID_ORDER_FLAGS";
    case RejectReason::INVALID_QUANTITY: return "INVALID_QUANTITY";
    case RejectReason::INVALID_LIMIT_PRICE: return "INVALID_LIMIT_PRICE";
    case RejectReason::MARKET_ORDER_CANNOT_BE_GTC: return "MARKET_ORDER_CANNOT_BE_GTC";
    case RejectReason::POST_ONLY_REQUIRES_GTC_LIMIT:
        return "POST_ONLY_REQUIRES_GTC_LIMIT";
    case RejectReason::POST_ONLY_WOULD_TRADE: return "POST_ONLY_WOULD_TRADE";
    case RejectReason::FOK_NOT_FILLABLE: return "FOK_NOT_FILLABLE";
    case RejectReason::RISK_KILL_SWITCH: return "RISK_KILL_SWITCH";
    case RejectReason::RISK_MAX_ORDER_QUANTITY: return "RISK_MAX_ORDER_QUANTITY";
    case RejectReason::RISK_PRICE_OUT_OF_RANGE: return "RISK_PRICE_OUT_OF_RANGE";
    case RejectReason::RISK_MAX_NOTIONAL: return "RISK_MAX_NOTIONAL";
    case RejectReason::RISK_MAX_ACTIVE_ORDERS: return "RISK_MAX_ACTIVE_ORDERS";
    case RejectReason::ORDER_NOT_FOUND: return "ORDER_NOT_FOUND";
    case RejectReason::INGRESS_QUEUE_FULL: return "INGRESS_QUEUE_FULL";
    case RejectReason::ENGINE_SHUTTING_DOWN: return "ENGINE_SHUTTING_DOWN";
    case RejectReason::JOURNAL_ERROR: return "JOURNAL_ERROR";
    }
    return "UNKNOWN_REJECT_REASON";
}

const char* toString(CommandType command) noexcept {
    switch (command) {
    case CommandType::NEW_ORDER: return "NEW_ORDER";
    case CommandType::CANCEL_ORDER: return "CANCEL_ORDER";
    }
    return "UNKNOWN_COMMAND";
}

} // namespace lob
