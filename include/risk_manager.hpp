#pragma once
// risk_manager.hpp
//
// Small, deterministic pre-trade risk layer. The defaults are
// intentionally permissive so legacy examples keep working, while
// applications can tighten limits or activate a kill switch.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "execution_report.hpp"
#include "order.hpp"

namespace lob {

struct RiskLimits {
    uint32_t max_order_quantity = std::numeric_limits<uint32_t>::max();
    int64_t min_limit_price = 1;
    int64_t max_limit_price = std::numeric_limits<int64_t>::max();
    uint64_t max_order_notional = std::numeric_limits<uint64_t>::max();
    size_t max_active_orders = std::numeric_limits<size_t>::max();
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits = {}) : limits_(limits) {}

    RejectReason validate(const Order& order, size_t active_orders,
                          bool may_add_active_order) const noexcept;

    void setKillSwitch(bool enabled) noexcept {
        kill_switch_.store(enabled, std::memory_order_release);
    }
    bool killSwitch() const noexcept {
        return kill_switch_.load(std::memory_order_acquire);
    }

    const RiskLimits& limits() const noexcept { return limits_; }
    void setLimits(const RiskLimits& limits) noexcept { limits_ = limits; }

private:
    RiskLimits limits_;
    std::atomic<bool> kill_switch_{false};
};

} // namespace lob
