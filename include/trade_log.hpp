#pragma once
// trade_log.hpp
//
// Bounded "last N trades" history per instrument, backed by
// std::deque<Trade>.
//
// WHY std::deque HERE SPECIFICALLY
// ----------------------------------
// This is a genuinely different access pattern from the order book's
// std::map/std::list/std::unordered_map, and a genuinely different
// container is the right tool:
//   - We push a new trade at the BACK every time the matching engine
//     produces one (newest-last).
//   - Once the log exceeds its cap, we drop the OLDEST trade from the
//     FRONT.
//   - Callers (e.g. a market-data viewer) want to iterate the most
//     recent K trades in order, and index into the log ("the 3rd most
//     recent trade") is genuinely useful for a UI/replay tool.
//
// std::deque gives O(1) push_back AND O(1) pop_front simultaneously,
// plus O(1) random access via operator[] -- exactly this shape.
//   - std::vector would need to shift every element on pop_front
//     (O(n)), or you'd have to fake a ring buffer yourself.
//   - std::list (used for price-level FIFOs, where we need O(1)
//     erase from the *middle* via a stored iterator for cancellation)
//     has no O(1) random access, which the "show me trade #k" query
//     needs, and has worse cache locality for straight-line iteration
//     since every node is a separate heap allocation.
// So: std::list for the order book's cancel-anywhere FIFOs,
// std::deque for this append/expire-oldest/random-access log. Two
// different jobs, two different containers -- not std::deque
// "instead of" std::list, but genuinely alongside it.

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "symbol.hpp"
#include "trade.hpp"

namespace lob {

class TradeLog {
public:
    explicit TradeLog(size_t max_per_symbol = 200) : max_per_symbol_(max_per_symbol) {}

    void record(const Symbol& symbol, const Trade& trade) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::deque<Trade>& dq = by_symbol_[symbol];
        dq.push_back(trade);
        if (dq.size() > max_per_symbol_) {
            dq.pop_front(); // O(1): the whole reason this is a deque, not a vector
        }
    }

    // Most recent trades for `symbol`, oldest first, at most `count`.
    std::vector<Trade> recent(const Symbol& symbol, size_t count) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = by_symbol_.find(symbol);
        if (it == by_symbol_.end()) return {};
        const std::deque<Trade>& dq = it->second;
        const size_t n = std::min(count, dq.size());
        // O(1) random access via deque::operator[] -- this is the
        // query a std::list could not answer without an O(n) walk.
        std::vector<Trade> out;
        out.reserve(n);
        for (size_t i = dq.size() - n; i < dq.size(); ++i) out.push_back(dq[i]);
        return out;
    }

    size_t count(const Symbol& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = by_symbol_.find(symbol);
        return it == by_symbol_.end() ? 0 : it->second.size();
    }

private:
    mutable std::mutex mutex_;
    size_t max_per_symbol_;
    std::unordered_map<Symbol, std::deque<Trade>> by_symbol_;
};

} // namespace lob
