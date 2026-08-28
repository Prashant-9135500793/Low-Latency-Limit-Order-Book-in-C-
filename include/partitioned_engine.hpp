#pragma once
// partitioned_engine.hpp
//
// Production-shaped multi-instrument pipeline:
//   many producers -> one bounded queue per shard -> one owner thread
//   per shard -> many single-threaded MatchingEngine instances.
//
// This is deliberately different from letting several workers pop a
// shared queue and race for a per-book mutex. A shared worker pool can
// pop two same-symbol orders in FIFO order but acquire the book lock in
// the opposite order, silently changing price-time priority. Here,
// each shard has exactly one consumer and processes its queue strictly
// FIFO. The queue's insertion linearization point defines arrival order
// for concurrent producers; different shards still run in parallel.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "execution_report.hpp"
#include "latency_histogram.hpp"
#include "matching_engine.hpp"
#include "mpmc_queue.hpp"
#include "risk_manager.hpp"
#include "symbol.hpp"
#include "trade.hpp"
#include "trade_log.hpp"

namespace lob {

struct PartitionedCommandResult {
    ExecutionReport report;
    std::vector<Trade> trades;
};

struct PartitionedEngineStats {
    uint64_t submitted = 0;
    uint64_t processed = 0;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t trades = 0;
    uint64_t pending = 0;
    size_t max_queue_high_watermark = 0;
    LatencySnapshot end_to_end_latency;
};

class PartitionedMatchingEngine {
public:
    struct Snapshot {
        bool exists = false;
        std::optional<int64_t> best_bid;
        std::optional<int64_t> best_ask;
        size_t order_count = 0;
        size_t buy_depth = 0;
        size_t sell_depth = 0;
        uint64_t sequence = 0;
    };

    explicit PartitionedMatchingEngine(size_t shard_count = 8,
                                       size_t queue_capacity_per_shard = 4096,
                                       RiskLimits risk_limits = {},
                                       size_t trade_log_depth_per_symbol = 200)
        : shard_count_(shard_count), risk_limits_(risk_limits),
          trade_log_(trade_log_depth_per_symbol) {
        if (shard_count_ == 0 || (shard_count_ & (shard_count_ - 1)) != 0) {
            throw std::invalid_argument("shard_count must be a non-zero power of two");
        }
        shards_.reserve(shard_count_);
        for (size_t i = 0; i < shard_count_; ++i) {
            shards_.push_back(std::make_unique<Shard>(queue_capacity_per_shard));
        }
        try {
            for (size_t i = 0; i < shard_count_; ++i) {
                shards_[i]->worker = std::thread([this, i] { workerLoop(i); });
            }
        } catch (...) {
            accepting_.store(false, std::memory_order_release);
            for (auto& shard : shards_) shard->queue.close();
            for (auto& shard : shards_) {
                if (shard->worker.joinable()) shard->worker.join();
            }
            throw;
        }
    }

    ~PartitionedMatchingEngine() { shutdown(); }

    PartitionedMatchingEngine(const PartitionedMatchingEngine&) = delete;
    PartitionedMatchingEngine& operator=(const PartitionedMatchingEngine&) = delete;

    // Blocking, fire-and-forget ingress with bounded-queue backpressure.
    bool enqueue(const Symbol& symbol, Order order) {
        Command command;
        command.type = CommandType::NEW_ORDER;
        command.symbol = symbol;
        command.order = order;
        command.enqueued_at = Clock::now();
        return enqueueCommand(std::move(command), /*blocking=*/true);
    }

    bool enqueueCancel(const Symbol& symbol, uint64_t order_id) {
        Command command;
        command.type = CommandType::CANCEL_ORDER;
        command.symbol = symbol;
        command.order.id = order_id;
        command.enqueued_at = Clock::now();
        return enqueueCommand(std::move(command), /*blocking=*/true);
    }

    // Non-blocking ingress. Returns false when the selected shard's
    // queue is full or the engine is shutting down.
    bool tryEnqueue(const Symbol& symbol, Order order) {
        Command command;
        command.type = CommandType::NEW_ORDER;
        command.symbol = symbol;
        command.order = order;
        command.enqueued_at = Clock::now();
        return enqueueCommand(std::move(command), /*blocking=*/false);
    }

    // Management/test API that returns the execution report and trades.
    // Queue insertion may block under backpressure; processing is async.
    std::future<PartitionedCommandResult> submit(const Symbol& symbol, Order order) {
        auto completion = std::make_shared<std::promise<PartitionedCommandResult>>();
        auto future = completion->get_future();
        Command command;
        command.type = CommandType::NEW_ORDER;
        command.symbol = symbol;
        command.order = order;
        command.completion = completion;
        command.enqueued_at = Clock::now();
        if (!enqueueCommand(std::move(command), /*blocking=*/true)) {
            completeRejected(completion, order.id, CommandType::NEW_ORDER,
                             RejectReason::ENGINE_SHUTTING_DOWN);
        }
        return future;
    }

    std::future<PartitionedCommandResult> cancel(const Symbol& symbol, uint64_t order_id) {
        auto completion = std::make_shared<std::promise<PartitionedCommandResult>>();
        auto future = completion->get_future();
        Command command;
        command.type = CommandType::CANCEL_ORDER;
        command.symbol = symbol;
        command.order.id = order_id;
        command.completion = completion;
        command.enqueued_at = Clock::now();
        if (!enqueueCommand(std::move(command), /*blocking=*/true)) {
            completeRejected(completion, order_id, CommandType::CANCEL_ORDER,
                             RejectReason::ENGINE_SHUTTING_DOWN);
        }
        return future;
    }

    void waitUntilIdle() {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        idle_cv_.wait(lock, [&] { return pending_.load(std::memory_order_acquire) == 0; });
    }

    void shutdown() noexcept {
        bool expected = true;
        if (!accepting_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }
        for (auto& shard : shards_) shard->queue.close();
        for (auto& shard : shards_) {
            if (shard->worker.joinable()) shard->worker.join();
        }
        idle_cv_.notify_all();
    }

    Snapshot snapshot(const Symbol& symbol) const {
        const Shard& shard = shardFor(symbol);
        std::lock_guard<std::mutex> lock(shard.state_mutex);
        const auto it = shard.engines.find(symbol);
        if (it == shard.engines.end()) return {};
        Snapshot snapshot;
        snapshot.exists = true;
        snapshot.best_bid = it->second.book().bestBid();
        snapshot.best_ask = it->second.book().bestAsk();
        snapshot.order_count = it->second.book().orderCount();
        snapshot.buy_depth = it->second.book().depth(Side::BUY);
        snapshot.sell_depth = it->second.book().depth(Side::SELL);
        snapshot.sequence = it->second.sequence();
        return snapshot;
    }

    bool validate(const Symbol& symbol) const {
        const Shard& shard = shardFor(symbol);
        std::lock_guard<std::mutex> lock(shard.state_mutex);
        const auto it = shard.engines.find(symbol);
        return it == shard.engines.end() || it->second.book().checkInvariants();
    }

    PartitionedEngineStats stats() const {
        PartitionedEngineStats out;
        out.submitted = submitted_.load(std::memory_order_relaxed);
        out.processed = processed_.load(std::memory_order_relaxed);
        out.accepted = accepted_.load(std::memory_order_relaxed);
        out.rejected = rejected_.load(std::memory_order_relaxed);
        out.trades = trades_.load(std::memory_order_relaxed);
        out.pending = pending_.load(std::memory_order_relaxed);
        for (const auto& shard : shards_) {
            out.max_queue_high_watermark =
                std::max(out.max_queue_high_watermark, shard->queue.highWatermark());
        }
        out.end_to_end_latency = latency_.snapshot();
        return out;
    }

    const TradeLog& tradeLog() const noexcept { return trade_log_; }
    size_t shardCount() const noexcept { return shard_count_; }

private:
    using Clock = std::chrono::steady_clock;

    struct Command {
        CommandType type = CommandType::NEW_ORDER;
        Symbol symbol;
        Order order;
        std::shared_ptr<std::promise<PartitionedCommandResult>> completion;
        Clock::time_point enqueued_at{};
    };

    struct Shard {
        explicit Shard(size_t queue_capacity) : queue(queue_capacity) {}

        MPMCQueue<Command> queue;
        std::thread worker;
        mutable std::mutex state_mutex;
        std::unordered_map<Symbol, MatchingEngine> engines;
    };

    size_t shardIndex(const Symbol& symbol) const noexcept {
        return std::hash<Symbol>{}(symbol) & (shard_count_ - 1);
    }
    Shard& shardFor(const Symbol& symbol) { return *shards_[shardIndex(symbol)]; }
    const Shard& shardFor(const Symbol& symbol) const {
        return *shards_[shardIndex(symbol)];
    }

    bool enqueueCommand(Command command, bool blocking) {
        if (!accepting_.load(std::memory_order_acquire)) return false;
        Shard& shard = shardFor(command.symbol);
        pending_.fetch_add(1, std::memory_order_acq_rel);
        const bool pushed = blocking ? shard.queue.push(std::move(command))
                                     : shard.queue.try_push(std::move(command));
        if (!pushed) {
            finishOnePending();
            return false;
        }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static void completeRejected(
        const std::shared_ptr<std::promise<PartitionedCommandResult>>& completion,
        uint64_t order_id, CommandType command, RejectReason reason) {
        PartitionedCommandResult result;
        result.report.order_id = order_id;
        result.report.command = command;
        result.report.status = ExecutionStatus::REJECTED;
        result.report.reject_reason = reason;
        completion->set_value(std::move(result));
    }

    void workerLoop(size_t shard_index) {
        Shard& shard = *shards_[shard_index];
        Command command;
        while (shard.queue.pop(command)) {
            PartitionedCommandResult result;
            {
                std::lock_guard<std::mutex> lock(shard.state_mutex);
                auto [it, inserted] = shard.engines.try_emplace(command.symbol, risk_limits_);
                (void)inserted;
                MatchingEngine& engine = it->second;
                if (command.type == CommandType::NEW_ORDER) {
                    result.report = engine.processOrder(command.order, result.trades);
                } else {
                    result.report = engine.cancel(command.order.id);
                }
            }

            for (const Trade& trade : result.trades) trade_log_.record(command.symbol, trade);
            processed_.fetch_add(1, std::memory_order_relaxed);
            if (result.report.accepted()) accepted_.fetch_add(1, std::memory_order_relaxed);
            else rejected_.fetch_add(1, std::memory_order_relaxed);
            trades_.fetch_add(result.trades.size(), std::memory_order_relaxed);

            const auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now() - command.enqueued_at)
                                     .count();
            latency_.record(latency < 0 ? 0 : static_cast<uint64_t>(latency));

            if (command.completion) command.completion->set_value(std::move(result));
            finishOnePending();
        }
    }

    void finishOnePending() noexcept {
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            idle_cv_.notify_all();
        }
    }

    size_t shard_count_;
    RiskLimits risk_limits_;
    std::vector<std::unique_ptr<Shard>> shards_;
    TradeLog trade_log_;
    AtomicLatencyHistogram latency_;

    std::atomic<bool> accepting_{true};
    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> processed_{0};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> trades_{0};
    std::atomic<uint64_t> pending_{0};
    mutable std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
};

} // namespace lob
