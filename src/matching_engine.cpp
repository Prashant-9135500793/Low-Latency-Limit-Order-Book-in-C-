#include "matching_engine.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace lob {

RejectReason MatchingEngine::validateStatic(const Order& order) const noexcept {
    if (order.id == 0) return RejectReason::INVALID_ORDER_ID;
    if (!isValidSide(order.side)) return RejectReason::INVALID_SIDE;
    if (!isValidOrderType(order.type)) return RejectReason::INVALID_ORDER_TYPE;
    if (!isValidTimeInForce(order.time_in_force)) {
        return RejectReason::INVALID_TIME_IN_FORCE;
    }

    constexpr uint8_t kKnownFlags = static_cast<uint8_t>(OrderFlags::POST_ONLY);
    if ((static_cast<uint8_t>(order.flags) & ~kKnownFlags) != 0) {
        return RejectReason::INVALID_ORDER_FLAGS;
    }
    if (order.quantity == 0) return RejectReason::INVALID_QUANTITY;
    if (order.type == OrderType::LIMIT && order.price <= 0) {
        return RejectReason::INVALID_LIMIT_PRICE;
    }
    if (order.type == OrderType::MARKET && order.time_in_force == TimeInForce::GTC) {
        return RejectReason::MARKET_ORDER_CANNOT_BE_GTC;
    }
    if (hasFlag(order.flags, OrderFlags::POST_ONLY) &&
        (order.type != OrderType::LIMIT || order.time_in_force != TimeInForce::GTC)) {
        return RejectReason::POST_ONLY_REQUIRES_GTC_LIMIT;
    }
    return RejectReason::NONE;
}

ExecutionReport MatchingEngine::rejectedReport(const Order& order, uint64_t sequence,
                                                RejectReason reason) noexcept {
    ++rejected_orders_;
    ExecutionReport report;
    report.order_id = order.id;
    report.sequence = sequence;
    report.command = CommandType::NEW_ORDER;
    report.status = ExecutionStatus::REJECTED;
    report.reject_reason = reason;
    report.requested_quantity = order.quantity;
    report.remaining_quantity = order.quantity;
    return report;
}

ExecutionReport MatchingEngine::processOrder(Order order, std::vector<Trade>& out_trades) {
    const uint64_t event_sequence = ++sequence_;
    const size_t trades_before = out_trades.size();

    if (const RejectReason reason = validateStatic(order); reason != RejectReason::NONE) {
        return rejectedReport(order, event_sequence, reason);
    }
    if (seen_order_ids_.contains(order.id)) {
        return rejectedReport(order, event_sequence, RejectReason::DUPLICATE_ORDER_ID);
    }

    const uint64_t executable = book_.executableQuantity(order, order.quantity);
    const bool may_add_active_order =
        order.type == OrderType::LIMIT && order.time_in_force == TimeInForce::GTC &&
        executable < order.quantity;
    if (const RejectReason reason =
            risk_.validate(order, book_.orderCount(), may_add_active_order);
        reason != RejectReason::NONE) {
        return rejectedReport(order, event_sequence, reason);
    }

    if (hasFlag(order.flags, OrderFlags::POST_ONLY) && book_.wouldCross(order)) {
        return rejectedReport(order, event_sequence, RejectReason::POST_ONLY_WOULD_TRADE);
    }
    if (order.time_in_force == TimeInForce::FOK && executable < order.quantity) {
        return rejectedReport(order, event_sequence, RejectReason::FOK_NOT_FILLABLE);
    }

    // From this point the command is accepted. The client order id is
    // consumed for the lifetime of this engine even when an IOC order
    // expires without a fill, preventing ambiguous audit/replay state.
    seen_order_ids_.insert(order.id);
    ++accepted_orders_;
    order.sequence = event_sequence;

    const uint32_t requested_quantity = order.quantity;
    const Side opposite = order.side == Side::BUY ? Side::SELL : Side::BUY;

    while (order.quantity > 0 && !book_.emptySide(opposite)) {
        const Order* best = book_.frontOfBest(opposite);
        assert(best != nullptr);

        const bool crosses = order.type == OrderType::MARKET ||
                             (order.side == Side::BUY ? order.price >= best->price
                                                      : order.price <= best->price);
        if (!crosses) break;

        const uint32_t traded_quantity = std::min(order.quantity, best->quantity);
        Trade trade;
        trade.trade_id = next_trade_id_++;
        trade.price = best->price;
        trade.quantity = traded_quantity;
        trade.sequence = order.sequence;
        if (order.side == Side::BUY) {
            trade.buy_order_id = order.id;
            trade.sell_order_id = best->id;
        } else {
            trade.buy_order_id = best->id;
            trade.sell_order_id = order.id;
        }
        out_trades.push_back(trade);

        book_.reduceFront(opposite, traded_quantity);
        order.quantity -= traded_quantity;
    }

    const uint32_t executed_quantity = requested_quantity - order.quantity;
    const bool should_rest = order.quantity > 0 && order.type == OrderType::LIMIT &&
                             order.time_in_force == TimeInForce::GTC;
    if (should_rest) {
        const bool inserted = book_.addOrder(order);
        assert(inserted && "validated order must be insertable");
        (void)inserted;
    }

    ExecutionReport report;
    report.order_id = order.id;
    report.sequence = event_sequence;
    report.command = CommandType::NEW_ORDER;
    report.reject_reason = RejectReason::NONE;
    report.requested_quantity = requested_quantity;
    report.executed_quantity = executed_quantity;
    report.remaining_quantity = order.quantity;
    report.trade_count = static_cast<uint32_t>(out_trades.size() - trades_before);

    if (order.quantity == 0) {
        report.status = ExecutionStatus::FILLED;
    } else if (should_rest) {
        report.status = executed_quantity == 0 ? ExecutionStatus::RESTING
                                               : ExecutionStatus::PARTIALLY_FILLED;
    } else {
        report.status = executed_quantity == 0 ? ExecutionStatus::EXPIRED
                                               : ExecutionStatus::PARTIALLY_FILLED;
    }

    return report;
}

ExecutionReport MatchingEngine::cancel(uint64_t order_id) {
    ExecutionReport report;
    report.order_id = order_id;
    report.sequence = ++sequence_;
    report.command = CommandType::CANCEL_ORDER;

    Order canceled_order;
    if (order_id == 0 || !book_.cancelOrder(order_id, &canceled_order)) {
        report.status = ExecutionStatus::REJECTED;
        report.reject_reason = order_id == 0 ? RejectReason::INVALID_ORDER_ID
                                             : RejectReason::ORDER_NOT_FOUND;
        return report;
    }

    ++canceled_orders_;
    report.status = ExecutionStatus::CANCELED;
    report.reject_reason = RejectReason::NONE;
    report.requested_quantity = canceled_order.quantity;
    report.remaining_quantity = 0;
    return report;
}

} // namespace lob
