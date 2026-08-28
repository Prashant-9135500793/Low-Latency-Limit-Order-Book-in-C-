// market_viewer.cpp
//
// Drains trades and execution reports from the mmap SPSC protocol and
// displays top-of-book plus operational counters. Use --quiet for a
// high-volume run that still drains every event without printing each.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "execution_report.hpp"
#include "shared_memory.hpp"
#include "shared_types.hpp"
#include "timestamp.hpp"

using namespace lob;

int main(int argc, char** argv) {
    std::string path = "/tmp/lob_shared_memory.dat";
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--path" && i + 1 < argc) path = argv[++i];
        else if (arg == "--quiet") quiet = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 1;
        }
    }

    SharedMemoryRegion region;
    std::printf("[market_viewer] opening shared memory at %s ...\n", path.c_str());
    for (int attempt = 0; !region.is_mapped(); ++attempt) {
        if (region.open(path, sizeof(SharedRegion))) break;
        if (attempt > 200) {
            std::fprintf(stderr, "[market_viewer] timed out waiting for shared memory\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    auto* shared = reinterpret_cast<SharedRegion*>(region.data());
    while (shared->header.state.load(std::memory_order_acquire) !=
           static_cast<uint32_t>(RegionState::READY)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const uint64_t magic = shared->header.magic.load(std::memory_order_acquire);
    const uint64_t region_size = shared->header.region_size.load(std::memory_order_acquire);
    const uint32_t version = shared->header.version.load(std::memory_order_acquire);
    if (magic != kSharedMemoryMagic || version != kSharedMemoryVersion ||
        region_size != sizeof(SharedRegion)) {
        std::fprintf(stderr,
                     "[market_viewer] protocol mismatch: magic=0x%llx version=%u/%u "
                     "region=%llu/%zu\n",
                     static_cast<unsigned long long>(magic), version, kSharedMemoryVersion,
                     static_cast<unsigned long long>(region_size), sizeof(SharedRegion));
        return 1;
    }
    shared->header.viewer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    std::printf("[market_viewer] connected to protocol v%u%s.\n", version,
                quiet ? " (quiet mode)" : "");

    uint64_t trades_seen = 0;
    uint64_t reports_seen = 0;
    uint64_t rejected_seen = 0;
    auto last_book_print = std::chrono::steady_clock::now();

    while (true) {
        bool got_any = false;

        Trade trade;
        while (shared->trade_queue.try_pop(trade)) {
            got_any = true;
            ++trades_seen;
            if (!quiet) {
                std::printf("TRADE #%llu buy=%llu sell=%llu price=%lld qty=%u seq=%llu\n",
                            static_cast<unsigned long long>(trade.trade_id),
                            static_cast<unsigned long long>(trade.buy_order_id),
                            static_cast<unsigned long long>(trade.sell_order_id),
                            static_cast<long long>(trade.price), trade.quantity,
                            static_cast<unsigned long long>(trade.sequence));
            }
        }

        ExecutionReport report;
        while (shared->report_queue.try_pop(report)) {
            got_any = true;
            ++reports_seen;
            if (!report.accepted()) ++rejected_seen;
            if (!quiet) {
                std::printf(
                    "REPORT seq=%llu cmd=%s order=%llu status=%s reason=%s "
                    "requested=%u executed=%u remaining=%u trades=%u\n",
                    static_cast<unsigned long long>(report.sequence),
                    toString(report.command),
                    static_cast<unsigned long long>(report.order_id),
                    toString(report.status), toString(report.reject_reason),
                    report.requested_quantity, report.executed_quantity,
                    report.remaining_quantity, report.trade_count);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_book_print > std::chrono::milliseconds(500)) {
            shared->header.viewer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
            const int64_t bid = shared->header.best_bid.load(std::memory_order_relaxed);
            const int64_t ask = shared->header.best_ask.load(std::memory_order_relaxed);
            const bool has_bid = bid != SharedHeader::kNoBid;
            const bool has_ask = ask != SharedHeader::kNoAsk;
            const std::string bid_text = has_bid ? std::to_string(bid) : "-";
            const std::string ask_text = has_ask ? std::to_string(ask) : "-";
            const std::string spread_text =
                has_bid && has_ask ? std::to_string(ask - bid) : "-";
            const uint64_t current_ns = now_ns();
            const uint64_t engine_heartbeat =
                shared->header.engine_heartbeat_ns.load(std::memory_order_relaxed);
            const uint64_t engine_age_ms =
                current_ns >= engine_heartbeat ? (current_ns - engine_heartbeat) / 1'000'000 : 0;
            std::printf(
                "-- book bid=%s ask=%s spread=%s | processed=%llu accepted=%llu "
                "rejected=%llu trades=%llu seq=%llu | q(order/trade/report)=%zu/%zu/%zu "
                "engine-heartbeat-age=%llums --\n",
                bid_text.c_str(), ask_text.c_str(), spread_text.c_str(),
                static_cast<unsigned long long>(
                    shared->header.processed_commands.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    shared->header.accepted_commands.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    shared->header.rejected_commands.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    shared->header.trades_generated.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    shared->header.last_sequence.load(std::memory_order_relaxed)),
                shared->order_queue.size(), shared->trade_queue.size(),
                shared->report_queue.size(),
                static_cast<unsigned long long>(engine_age_ms));
            last_book_print = now;
        }

        if (!got_any) {
            if (shared->header.engine_done.load(std::memory_order_acquire) &&
                shared->trade_queue.empty() && shared->report_queue.empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    shared->header.viewer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    std::printf(
        "[market_viewer] engine finished; reports=%llu rejected=%llu trades=%llu\n",
        static_cast<unsigned long long>(reports_seen),
        static_cast<unsigned long long>(rejected_seen),
        static_cast<unsigned long long>(trades_seen));
    return 0;
}
