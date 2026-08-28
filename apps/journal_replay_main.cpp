// journal_replay_main.cpp
//
// Offline journal validator and recovery inspector. It verifies every
// fixed-width record (magic/version/ordinal/CRC), replays commands through
// the same MatchingEngine code, then prints reconstructed top-of-book and
// depth. Use the same risk limits that were active when the journal was
// written so rejected/accepted decisions remain deterministic.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "journal.hpp"
#include "matching_engine.hpp"

using namespace lob;

namespace {

struct Args {
    std::string journal_path;
    size_t depth = 10;
    RiskLimits risk_limits;
};

uint64_t parseU64(const char* value, const char* flag) {
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || value[0] == '-') {
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

        if (flag == "--journal") args.journal_path = next("--journal");
        else if (flag == "--depth") {
            const uint64_t value = parseU64(next("--depth"), flag.c_str());
            if (value > std::numeric_limits<size_t>::max()) {
                std::fprintf(stderr, "--depth exceeds size_t\n");
                std::exit(1);
            }
            args.depth = static_cast<size_t>(value);
        } else if (flag == "--max-order-qty") {
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
            const uint64_t value = parseU64(next("--max-active-orders"), flag.c_str());
            if (value > std::numeric_limits<size_t>::max()) {
                std::fprintf(stderr, "--max-active-orders exceeds size_t\n");
                std::exit(1);
            }
            args.risk_limits.max_active_orders = static_cast<size_t>(value);
        } else if (flag == "--help") {
            std::printf(
                "Usage: journal_replay_main --journal PATH [--depth N]\n"
                "       [--max-order-qty N] [--min-price N] [--max-price N]\n"
                "       [--max-notional N] [--max-active-orders N]\n");
            std::exit(0);
        } else if (!flag.empty() && flag[0] != '-' && args.journal_path.empty()) {
            args.journal_path = flag;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
            std::exit(1);
        }
    }

    if (args.journal_path.empty()) {
        std::fprintf(stderr, "journal path is required (use --journal PATH)\n");
        std::exit(1);
    }
    if (args.risk_limits.min_limit_price > args.risk_limits.max_limit_price) {
        std::fprintf(stderr, "min price cannot exceed max price\n");
        std::exit(1);
    }
    return args;
}

void printLevels(const char* label, const std::vector<LevelSnapshot>& levels) {
    std::printf("%s (%zu levels shown)\n", label, levels.size());
    if (levels.empty()) {
        std::printf("  <empty>\n");
        return;
    }
    for (const LevelSnapshot& level : levels) {
        std::printf("  price=%lld quantity=%llu orders=%zu\n",
                    static_cast<long long>(level.price),
                    static_cast<unsigned long long>(level.total_quantity),
                    level.order_count);
    }
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    MatchingEngine engine(args.risk_limits);
    const JournalReplayResult result = EventJournal::replay(args.journal_path, engine);
    if (!result.ok) {
        std::fprintf(stderr, "journal validation/replay failed at byte %llu: %s\n",
                     static_cast<unsigned long long>(result.error_offset),
                     result.error.c_str());
        return 1;
    }

    std::printf("Journal: %s\n", args.journal_path.c_str());
    std::printf(
        "records=%llu new_orders=%llu cancels=%llu accepted=%llu rejected=%llu "
        "trades=%llu last_ordinal=%llu\n",
        static_cast<unsigned long long>(result.records),
        static_cast<unsigned long long>(result.new_orders),
        static_cast<unsigned long long>(result.cancels),
        static_cast<unsigned long long>(result.accepted),
        static_cast<unsigned long long>(result.rejected),
        static_cast<unsigned long long>(result.trades),
        static_cast<unsigned long long>(result.last_ordinal));

    const auto bid = engine.book().bestBid();
    const auto ask = engine.book().bestAsk();
    std::printf("recovered_sequence=%llu active_orders=%zu best_bid=",
                static_cast<unsigned long long>(engine.sequence()),
                engine.book().orderCount());
    if (bid) std::printf("%lld", static_cast<long long>(*bid));
    else std::printf("-");
    std::printf(" best_ask=");
    if (ask) std::printf("%lld", static_cast<long long>(*ask));
    else std::printf("-");
    if (bid && ask) {
        std::printf(" spread=%lld", static_cast<long long>(*ask - *bid));
    }
    std::printf(" invariants=%s\n", engine.book().checkInvariants() ? "PASS" : "FAIL");

    printLevels("BIDS (best first)", engine.book().topLevels(Side::BUY, args.depth));
    printLevels("ASKS (best first)", engine.book().topLevels(Side::SELL, args.depth));
    return engine.book().checkInvariants() ? 0 : 2;
}
