// feed_handler.cpp
//
// Producer process for the mmap SPSC architecture. It can generate the
// original deterministic GTC LIMIT flow, an advanced mixed flow
// (MARKET/IOC/FOK/post-only), or orders bounded by real AAPL daily data.
// Optional cancel commands exercise the command/report path as well.
//
// Usage:
//   feed_handler [--orders N] [--seed N] [--advanced-orders]
//                [--cancel-every N] [--path P]
//   feed_handler --real-data [--data-file P] [--orders-per-day N]
//                [--seed N] [--path P]

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "csv_reader.hpp"
#include "shared_memory.hpp"
#include "shared_types.hpp"
#include "timestamp.hpp"

using namespace lob;

namespace {

struct Args {
    uint64_t num_orders = 10000;
    uint64_t seed = 42;
    std::string path = "/tmp/lob_shared_memory.dat";
    bool real_data = false;
    bool advanced_orders = false;
    uint64_t cancel_every = 0;
    std::string data_file = "../data/AAPL.csv";
    uint64_t orders_per_day = 40;
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

        if (flag == "--orders") args.num_orders = parseU64(next("--orders"), flag.c_str());
        else if (flag == "--seed") args.seed = parseU64(next("--seed"), flag.c_str());
        else if (flag == "--path") args.path = next("--path");
        else if (flag == "--real-data") args.real_data = true;
        else if (flag == "--advanced-orders") args.advanced_orders = true;
        else if (flag == "--cancel-every") {
            args.cancel_every = parseU64(next("--cancel-every"), flag.c_str());
        } else if (flag == "--data-file") args.data_file = next("--data-file");
        else if (flag == "--orders-per-day") {
            args.orders_per_day = parseU64(next("--orders-per-day"), flag.c_str());
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
            std::exit(1);
        }
    }

    if (args.real_data && args.advanced_orders) {
        std::fprintf(stderr,
                     "--advanced-orders is a synthetic stress mode and cannot be combined "
                     "with --real-data\n");
        std::exit(1);
    }
    if (args.orders_per_day == 0) {
        std::fprintf(stderr, "--orders-per-day must be greater than zero\n");
        std::exit(1);
    }
    return args;
}

class SyntheticOrderGenerator {
public:
    SyntheticOrderGenerator(uint64_t seed, bool advanced)
        : rng_(seed), advanced_(advanced), price_dist_(9900, 10100), qty_dist_(1, 500),
          side_dist_(0, 1), policy_dist_(0, 99) {}

    Order next(uint64_t id) {
        Order order;
        order.id = id;
        order.side = side_dist_(rng_) == 0 ? Side::BUY : Side::SELL;
        order.type = OrderType::LIMIT;
        order.time_in_force = TimeInForce::GTC;
        order.price = price_dist_(rng_);
        order.quantity = qty_dist_(rng_);

        if (!advanced_) return order;

        const int policy = policy_dist_(rng_);
        if (policy < 5) {
            order.type = OrderType::MARKET;
            order.time_in_force = TimeInForce::IOC;
            order.price = 0;
        } else if (policy < 15) {
            order.time_in_force = TimeInForce::IOC;
        } else if (policy < 20) {
            order.time_in_force = TimeInForce::FOK;
        } else if (policy < 25) {
            order.flags = OrderFlags::POST_ONLY;
        }
        return order;
    }

private:
    std::mt19937_64 rng_;
    bool advanced_;
    std::uniform_int_distribution<int64_t> price_dist_;
    std::uniform_int_distribution<uint32_t> qty_dist_;
    std::uniform_int_distribution<int> side_dist_;
    std::uniform_int_distribution<int> policy_dist_;
};

class RealDataOrderGenerator {
public:
    RealDataOrderGenerator(std::vector<DailyBar> bars, uint64_t orders_per_day, uint64_t seed)
        : bars_(std::move(bars)), orders_per_day_(orders_per_day), rng_(seed) {}

    uint64_t totalOrders() const { return bars_.size() * orders_per_day_; }

    Order next(uint64_t id) {
        const DailyBar& bar = dayFor(id);
        const int64_t low_ticks = static_cast<int64_t>(bar.low * 100.0 + 0.5);
        const int64_t high_ticks = static_cast<int64_t>(bar.high * 100.0 + 0.5);
        const int64_t lo = std::min(low_ticks, high_ticks);
        const int64_t hi = std::max(low_ticks, high_ticks);

        // std::uniform_int_distribution accepts equal endpoints. Keeping
        // [lo, hi] unchanged is important: expanding a flat daily range
        // by one tick would violate real-data mode's bounding guarantee.
        std::uniform_int_distribution<int64_t> price_dist(lo, hi);
        std::uniform_int_distribution<int> side_dist(0, 1);
        const double volume_weight =
            std::min(1.0, static_cast<double>(bar.volume) / 100'000'000.0);
        std::uniform_int_distribution<uint32_t> qty_dist(
            1, static_cast<uint32_t>(50 + volume_weight * 450));

        Order order;
        order.id = id;
        order.side = side_dist(rng_) == 0 ? Side::BUY : Side::SELL;
        order.type = OrderType::LIMIT;
        order.time_in_force = TimeInForce::GTC;
        order.price = price_dist(rng_);
        order.quantity = qty_dist(rng_);
        return order;
    }

    const DailyBar& dayFor(uint64_t id) const {
        const size_t day_index = static_cast<size_t>((id - 1) / orders_per_day_);
        return bars_[std::min(day_index, bars_.size() - 1)];
    }

private:
    std::vector<DailyBar> bars_;
    uint64_t orders_per_day_;
    std::mt19937_64 rng_;
};

void pushMessage(SharedRegion& shared, const OrderMsg& message) {
    while (!shared.order_queue.try_push(message)) std::this_thread::yield();
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);

    SharedMemoryRegion region;
    std::printf("[feed_handler] opening shared memory at %s ...\n", args.path.c_str());
    for (int attempt = 0; !region.is_mapped(); ++attempt) {
        if (region.open(args.path, sizeof(SharedRegion))) break;
        if (attempt > 200) {
            std::fprintf(stderr,
                         "[feed_handler] timed out waiting for shared memory; "
                         "is matching_engine_main running?\n");
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
                     "[feed_handler] shared-memory protocol mismatch: version=%u/%u "
                     "region=%llu/%zu\n",
                     version, kSharedMemoryVersion,
                     static_cast<unsigned long long>(region_size), sizeof(SharedRegion));
        return 1;
    }
    shared->header.producer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);

    uint64_t orders_pushed = 0;
    uint64_t cancels_pushed = 0;
    const auto start = std::chrono::steady_clock::now();

    const auto sendOrder = [&](const Order& order) {
        OrderMsg message;
        message.type = OrderMsgType::NEW_ORDER;
        message.order = order;
        pushMessage(*shared, message);
        ++orders_pushed;
        if ((orders_pushed & 0xFFFU) == 0U) {
            shared->header.producer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
        }

        if (args.cancel_every > 0 && orders_pushed % args.cancel_every == 0) {
            OrderMsg cancel;
            cancel.type = OrderMsgType::CANCEL_ORDER;
            // Deterministic target from the recent past. It may already
            // be filled, which intentionally exercises ORDER_NOT_FOUND.
            cancel.order.id = std::max<uint64_t>(1, orders_pushed - args.cancel_every / 2);
            pushMessage(*shared, cancel);
            ++cancels_pushed;
        }
    };

    if (args.real_data) {
        std::vector<DailyBar> bars = loadDailyBars(args.data_file);
        if (bars.empty()) {
            std::fprintf(stderr,
                         "[feed_handler] could not load real data file '%s' (or it was "
                         "empty); real-data mode never falls back to synthetic input\n",
                         args.data_file.c_str());
            return 1;
        }
        RealDataOrderGenerator generator(std::move(bars), args.orders_per_day, args.seed);
        const uint64_t total = generator.totalOrders();
        std::printf(
            "[feed_handler] replaying %llu real trading days x %llu orders/day = "
            "%llu orders\n",
            static_cast<unsigned long long>(total / args.orders_per_day),
            static_cast<unsigned long long>(args.orders_per_day),
            static_cast<unsigned long long>(total));

        std::string first_date;
        std::string last_date;
        for (uint64_t id = 1; id <= total; ++id) {
            if (id == 1) first_date = generator.dayFor(id).date;
            last_date = generator.dayFor(id).date;
            sendOrder(generator.next(id));
        }
        std::printf("[feed_handler] replayed real trading days %s .. %s\n",
                    first_date.c_str(), last_date.c_str());
    } else {
        std::printf(
            "[feed_handler] generating %llu synthetic orders (seed=%llu, mode=%s, "
            "cancel_every=%llu)\n",
            static_cast<unsigned long long>(args.num_orders),
            static_cast<unsigned long long>(args.seed),
            args.advanced_orders ? "advanced-mix" : "GTC-limit",
            static_cast<unsigned long long>(args.cancel_every));
        SyntheticOrderGenerator generator(args.seed, args.advanced_orders);
        for (uint64_t id = 1; id <= args.num_orders; ++id) sendOrder(generator.next(id));
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    shared->header.producer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    shared->header.producer_done.store(1, std::memory_order_release);
    const uint64_t messages = orders_pushed + cancels_pushed;
    std::printf(
        "[feed_handler] pushed %llu orders + %llu cancels = %llu commands in %.4f s "
        "(%.0f commands/sec)\n",
        static_cast<unsigned long long>(orders_pushed),
        static_cast<unsigned long long>(cancels_pushed),
        static_cast<unsigned long long>(messages), seconds,
        seconds > 0 ? static_cast<double>(messages) / seconds : 0.0);
    return 0;
}
