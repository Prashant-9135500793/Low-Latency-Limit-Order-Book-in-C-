#pragma once
// shared_types.hpp
//
// Fixed, pointer-free shared-memory protocol for the three-process
// SPSC architecture. Version 2 adds execution reports and advanced
// order fields while preserving the same mmap + acquire/release model.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "execution_report.hpp"
#include "order.hpp"
#include "spsc_queue.hpp"
#include "trade.hpp"

namespace lob {

// Cross-process use relies on these atomics compiling to true hardware
// atomics rather than process-private library locks. The supported Linux
// x86_64/arm64 targets satisfy these checks.
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "shared-memory protocol requires lock-free 32-bit atomics");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "shared-memory protocol requires lock-free 64-bit atomics");
static_assert(std::atomic<int64_t>::is_always_lock_free,
              "shared-memory protocol requires lock-free 64-bit atomics");

enum class RegionState : uint32_t { NOT_READY = 0, READY = 1 };
enum class OrderMsgType : uint32_t { NEW_ORDER = 0, CANCEL_ORDER = 1 };

struct OrderMsg {
    OrderMsgType type = OrderMsgType::NEW_ORDER;
    Order order{}; // for CANCEL_ORDER, only order.id is meaningful
};
static_assert(std::is_trivially_copyable_v<OrderMsg>);

inline constexpr uint64_t kSharedMemoryMagic = 0x4C4F42'53505343ULL; // "LOBSPSC"
inline constexpr uint32_t kSharedMemoryVersion = 2;

inline constexpr size_t kOrderQueueCapacity = 1u << 16;
inline constexpr size_t kTradeQueueCapacity = 1u << 16;
inline constexpr size_t kReportQueueCapacity = 1u << 16;

struct SharedHeader {
    std::atomic<uint64_t> magic{0};
    std::atomic<uint64_t> region_size{0};
    std::atomic<uint32_t> version{0};
    std::atomic<uint32_t> state{static_cast<uint32_t>(RegionState::NOT_READY)};
    std::atomic<uint32_t> producer_done{0};
    std::atomic<uint32_t> engine_done{0};

    static constexpr int64_t kNoBid = INT64_MIN;
    static constexpr int64_t kNoAsk = INT64_MAX;
    std::atomic<int64_t> best_bid{kNoBid};
    std::atomic<int64_t> best_ask{kNoAsk};

    // Operational counters give the viewer a cheap health/telemetry
    // surface without exposing the engine's internal data structures.
    std::atomic<uint64_t> processed_commands{0};
    std::atomic<uint64_t> accepted_commands{0};
    std::atomic<uint64_t> rejected_commands{0};
    std::atomic<uint64_t> canceled_orders{0};
    std::atomic<uint64_t> trades_generated{0};
    std::atomic<uint64_t> last_sequence{0};

    // Linux steady-clock timestamps used as inexpensive process-health
    // heartbeats. They are advisory telemetry, never correctness inputs.
    std::atomic<uint64_t> engine_heartbeat_ns{0};
    std::atomic<uint64_t> producer_heartbeat_ns{0};
    std::atomic<uint64_t> viewer_heartbeat_ns{0};
};

struct SharedRegion {
    SharedHeader header;
    SPSCQueue<OrderMsg, kOrderQueueCapacity> order_queue;          // feed -> engine
    SPSCQueue<Trade, kTradeQueueCapacity> trade_queue;             // engine -> viewer
    SPSCQueue<ExecutionReport, kReportQueueCapacity> report_queue; // engine -> viewer
};

} // namespace lob
