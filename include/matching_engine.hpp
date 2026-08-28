#pragma once
// matching_engine.hpp
//
// Deterministic, single-writer matching engine with explicit execution
// reports, market/IOC/FOK/post-only semantics, duplicate-id protection,
// and configurable pre-trade risk checks.

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "execution_report.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "risk_manager.hpp"
#include "trade.hpp"

namespace lob {

class MatchingEngine {
public:
    explicit MatchingEngine(RiskLimits risk_limits = {}) : risk_(risk_limits) {}

    // Full professional-style API. Trades are appended to out_trades;
    // the report describes acceptance/rejection and the final state.
    ExecutionReport processOrder(Order order, std::vector<Trade>& out_trades);
    ExecutionReport cancel(uint64_t order_id);

    // Backward-compatible wrappers retained for the original demos and
    // benchmarks.
    size_t submitOrder(Order order, std::vector<Trade>& out_trades) {
        return processOrder(order, out_trades).trade_count;
    }
    bool cancelOrder(uint64_t order_id) {
        return cancel(order_id).status == ExecutionStatus::CANCELED;
    }

    const OrderBook& book() const { return book_; }
    OrderBook& bookForTesting() { return book_; }

    uint64_t sequence() const { return sequence_; }
    uint64_t tradeCount() const { return next_trade_id_ - 1; }
    uint64_t acceptedOrderCount() const { return accepted_orders_; }
    uint64_t rejectedOrderCount() const { return rejected_orders_; }
    uint64_t canceledOrderCount() const { return canceled_orders_; }

    RiskManager& riskManager() noexcept { return risk_; }
    const RiskManager& riskManager() const noexcept { return risk_; }

private:
    RejectReason validateStatic(const Order& order) const noexcept;
    ExecutionReport rejectedReport(const Order& order, uint64_t sequence,
                                   RejectReason reason) noexcept;

    OrderBook book_;
    RiskManager risk_;
    std::unordered_set<uint64_t> seen_order_ids_;
    uint64_t sequence_ = 0;
    uint64_t next_trade_id_ = 1;
    uint64_t accepted_orders_ = 0;
    uint64_t rejected_orders_ = 0;
    uint64_t canceled_orders_ = 0;
};

} // namespace lob
