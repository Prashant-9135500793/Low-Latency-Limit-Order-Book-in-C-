// matching_engine_main.cpp
//
// Three-process SPSC architecture's matching-engine process. Version 2
// adds explicit execution reports, pre-trade risk limits, and optional
// append-only journaling/recovery while retaining the mmap hot path.
//
// Usage:
//   matching_engine_main [--path P] [--idle-exit-ms N]
//                        [--journal P] [--recover] [--sync-journal]
//                        [--max-order-qty N] [--min-price N]
//                        [--max-price N] [--max-notional N]
//                        [--max-active-orders N]

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "execution_report.hpp"
#include "journal.hpp"
#include "matching_engine.hpp"
#include "shared_memory.hpp"
#include "shared_types.hpp"
#include "timestamp.hpp"

using namespace lob;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Args {
    std::string shared_memory_path = "/tmp/lob_shared_memory.dat";
    int idle_exit_ms = 2000;
    std::string journal_path;
    bool recover = false;
    bool sync_journal = false;
    RiskLimits risk_limits;
};

uint64_t parseU64(const char* value, const char* flag) {
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        std::fprintf(stderr, "invalid numeric value for %s: %s\n", flag, value);
        std::exit(1);
    }
    return static_cast<uint64_t>(parsed);
}

int64_t parseI64(const char* value, const char* flag) {
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        std::fprintf(stderr, "invalid numeric value for %s: %s\n", flag, value);
        std::exit(1);
    }
    return static_cast<int64_t>(parsed);
}

Args parseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (flag == "--path") args.shared_memory_path = next("--path");
        else if (flag == "--idle-exit-ms") {
            args.idle_exit_ms = static_cast<int>(parseI64(next("--idle-exit-ms"), flag.c_str()));
        } else if (flag == "--journal") args.journal_path = next("--journal");
        else if (flag == "--recover") args.recover = true;
        else if (flag == "--sync-journal") args.sync_journal = true;
        else if (flag == "--max-order-qty") {
            const uint64_t value = parseU64(next("--max-order-qty"), flag.c_str());
            if (value > std::numeric_limits<uint32_t>::max()) {
                std::fprintf(stderr, "--max-order-qty exceeds uint32 range\n");
                std::exit(1);
            }
            args.risk_limits.max_order_quantity = static_cast<uint32_t>(value);
        } else if (flag == "--min-price") {
            args.risk_limits.min_limit_price = parseI64(next("--min-price"), flag.c_str());
        } else if (flag == "--max-price") {
            args.risk_limits.max_limit_price = parseI64(next("--max-price"), flag.c_str());
        } else if (flag == "--max-notional") {
            args.risk_limits.max_order_notional =
                parseU64(next("--max-notional"), flag.c_str());
        } else if (flag == "--max-active-orders") {
            args.risk_limits.max_active_orders = static_cast<size_t>(
                parseU64(next("--max-active-orders"), flag.c_str()));
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
            std::exit(1);
        }
    }

    if (args.recover && args.journal_path.empty()) {
        std::fprintf(stderr, "--recover requires --journal PATH\n");
        std::exit(1);
    }
    if (args.risk_limits.min_limit_price > args.risk_limits.max_limit_price) {
        std::fprintf(stderr, "min price cannot exceed max price\n");
        std::exit(1);
    }
    return args;
}

void publishBook(const MatchingEngine& engine, SharedRegion& shared) {
    shared.header.best_bid.store(
        engine.book().bestBid().value_or(SharedHeader::kNoBid),
        std::memory_order_relaxed);
    shared.header.best_ask.store(
        engine.book().bestAsk().value_or(SharedHeader::kNoAsk),
        std::memory_order_relaxed);
    shared.header.last_sequence.store(engine.sequence(), std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    MatchingEngine engine(args.risk_limits);
    JournalReplayResult replay_result;
    if (args.recover) {
        replay_result = EventJournal::replay(args.journal_path, engine);
        if (!replay_result.ok) {
            std::fprintf(stderr,
                         "[matching_engine] journal recovery failed at byte %llu: %s\n",
                         static_cast<unsigned long long>(replay_result.error_offset),
                         replay_result.error.c_str());
            return 1;
        }
        std::printf(
            "[matching_engine] recovered %llu commands, %llu trades, sequence=%llu "
            "from %s\n",
            static_cast<unsigned long long>(replay_result.records),
            static_cast<unsigned long long>(replay_result.trades),
            static_cast<unsigned long long>(engine.sequence()), args.journal_path.c_str());
    }

    EventJournal journal;
    if (!args.journal_path.empty()) {
        const JournalOpenMode mode =
            args.recover ? JournalOpenMode::APPEND : JournalOpenMode::TRUNCATE;
        const JournalDurability durability = args.sync_journal
                                                 ? JournalDurability::FDATASYNC_EACH_RECORD
                                                 : JournalDurability::BUFFERED;
        if (!journal.open(args.journal_path, mode, durability)) {
            std::fprintf(stderr, "[matching_engine] cannot open journal: %s\n",
                         journal.lastError().c_str());
            return 1;
        }
        std::printf("[matching_engine] journal %s (%s durability)\n",
                    args.journal_path.c_str(),
                    args.sync_journal ? "fdatasync-per-record" : "buffered");
    }

    SharedMemoryRegion::remove_file(args.shared_memory_path);
    SharedMemoryRegion region;
    if (!region.create(args.shared_memory_path, sizeof(SharedRegion))) {
        std::fprintf(stderr, "[matching_engine] failed to create shared memory at %s\n",
                     args.shared_memory_path.c_str());
        return 1;
    }

    auto* shared = new (region.data()) SharedRegion();
    shared->header.magic.store(kSharedMemoryMagic, std::memory_order_relaxed);
    shared->header.region_size.store(sizeof(SharedRegion), std::memory_order_relaxed);
    shared->header.version.store(kSharedMemoryVersion, std::memory_order_relaxed);
    shared->header.engine_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    shared->header.processed_commands.store(replay_result.records, std::memory_order_relaxed);
    shared->header.accepted_commands.store(replay_result.accepted, std::memory_order_relaxed);
    shared->header.rejected_commands.store(replay_result.rejected, std::memory_order_relaxed);
    shared->header.canceled_orders.store(engine.canceledOrderCount(),
                                          std::memory_order_relaxed);
    shared->header.trades_generated.store(engine.tradeCount(), std::memory_order_relaxed);
    publishBook(engine, *shared);
    shared->header.state.store(static_cast<uint32_t>(RegionState::READY),
                               std::memory_order_release);

    std::printf("[matching_engine] shared memory protocol v%u ready at %s (%zu bytes)\n",
                kSharedMemoryVersion, args.shared_memory_path.c_str(), sizeof(SharedRegion));

    std::vector<Trade> trades;
    trades.reserve(8);
    auto last_activity = std::chrono::steady_clock::now();
    auto next_heartbeat = last_activity;
    bool journal_failed = false;

    while (!g_stop) {
        const auto loop_now = std::chrono::steady_clock::now();
        if (loop_now >= next_heartbeat) {
            shared->header.engine_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
            next_heartbeat = loop_now + std::chrono::milliseconds(100);
        }
        OrderMsg message;
        const bool got_one = shared->order_queue.try_pop(message);
        if (!got_one) {
            if (args.idle_exit_ms > 0 &&
                shared->header.producer_done.load(std::memory_order_acquire)) {
                const auto idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_activity);
                if (idle_for.count() > args.idle_exit_ms) break;
            }
            std::this_thread::yield();
            continue;
        }

        last_activity = std::chrono::steady_clock::now();

        if (journal.isOpen()) {
            const bool journaled = message.type == OrderMsgType::NEW_ORDER
                                       ? journal.appendNewOrder(message.order)
                                       : journal.appendCancel(message.order.id);
            if (!journaled) {
                std::fprintf(stderr,
                             "[matching_engine] fatal: command was not applied because "
                             "journal append failed: %s\n",
                             journal.lastError().c_str());
                journal_failed = true;
                break;
            }
        }

        trades.clear();
        ExecutionReport report;
        if (message.type == OrderMsgType::NEW_ORDER) {
            report = engine.processOrder(message.order, trades);
        } else {
            report = engine.cancel(message.order.id);
        }

        for (const Trade& trade : trades) {
            while (!shared->trade_queue.try_push(trade)) std::this_thread::yield();
        }
        while (!shared->report_queue.try_push(report)) std::this_thread::yield();

        shared->header.processed_commands.fetch_add(1, std::memory_order_relaxed);
        if (report.accepted()) {
            shared->header.accepted_commands.fetch_add(1, std::memory_order_relaxed);
        } else {
            shared->header.rejected_commands.fetch_add(1, std::memory_order_relaxed);
        }
        if (report.status == ExecutionStatus::CANCELED) {
            shared->header.canceled_orders.fetch_add(1, std::memory_order_relaxed);
        }
        shared->header.trades_generated.fetch_add(trades.size(), std::memory_order_relaxed);
        publishBook(engine, *shared);
    }

    if (journal.isOpen() && !args.sync_journal && !journal.sync()) {
        std::fprintf(stderr, "[matching_engine] final journal sync failed: %s\n",
                     journal.lastError().c_str());
        journal_failed = true;
    }

    shared->header.engine_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    shared->header.engine_done.store(1, std::memory_order_release);
    std::printf(
        "[matching_engine] processed=%llu accepted=%llu rejected=%llu canceled=%llu "
        "trades=%llu sequence=%llu\n",
        static_cast<unsigned long long>(
            shared->header.processed_commands.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            shared->header.accepted_commands.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            shared->header.rejected_commands.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            shared->header.canceled_orders.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            shared->header.trades_generated.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(engine.sequence()));
    std::printf("[matching_engine] final book: depth(BUY)=%zu depth(SELL)=%zu orders=%zu\n",
                engine.book().depth(Side::BUY), engine.book().depth(Side::SELL),
                engine.book().orderCount());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return journal_failed ? 2 : 0;
}
