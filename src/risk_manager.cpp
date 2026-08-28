#include "risk_manager.hpp"

namespace lob {

RejectReason RiskManager::validate(const Order& order, size_t active_orders,
                                   bool may_add_active_order) const noexcept {
    if (killSwitch()) return RejectReason::RISK_KILL_SWITCH;

    if (order.quantity > limits_.max_order_quantity) {
        return RejectReason::RISK_MAX_ORDER_QUANTITY;
    }

    if (order.type == OrderType::LIMIT) {
        if (order.price < limits_.min_limit_price || order.price > limits_.max_limit_price) {
            return RejectReason::RISK_PRICE_OUT_OF_RANGE;
        }

        const auto price = static_cast<uint64_t>(order.price);
        if (order.quantity != 0 &&
            price > limits_.max_order_notional / order.quantity) {
            return RejectReason::RISK_MAX_NOTIONAL;
        }
    }

    if (may_add_active_order && active_orders >= limits_.max_active_orders) {
        return RejectReason::RISK_MAX_ACTIVE_ORDERS;
    }

    return RejectReason::NONE;
}

} // namespace lob
