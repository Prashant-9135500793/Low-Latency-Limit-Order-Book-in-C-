#pragma once
// latency_histogram.hpp
//
// Lock-free, allocation-free logarithmic latency histogram. Buckets
// are powers of two nanoseconds; percentiles are therefore approximate
// upper bounds, but recording is cheap enough for the live pipeline.

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lob {

struct LatencySnapshot {
    uint64_t count = 0;
    uint64_t min_ns = 0;
    double mean_ns = 0.0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
};

class AtomicLatencyHistogram {
public:
    static constexpr size_t kBucketCount = 64;

    AtomicLatencyHistogram() {
        for (auto& bucket : buckets_) bucket.store(0, std::memory_order_relaxed);
    }

    void record(uint64_t nanoseconds) noexcept {
        const size_t bucket = bucketFor(nanoseconds);
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        total_ns_.fetch_add(nanoseconds, std::memory_order_relaxed);

        uint64_t current_min = min_ns_.load(std::memory_order_relaxed);
        while (nanoseconds < current_min &&
               !min_ns_.compare_exchange_weak(current_min, nanoseconds,
                                              std::memory_order_relaxed)) {
        }
        uint64_t current_max = max_ns_.load(std::memory_order_relaxed);
        while (nanoseconds > current_max &&
               !max_ns_.compare_exchange_weak(current_max, nanoseconds,
                                              std::memory_order_relaxed)) {
        }
    }

    LatencySnapshot snapshot() const noexcept {
        LatencySnapshot out;
        out.count = count_.load(std::memory_order_relaxed);
        if (out.count == 0) return out;

        out.min_ns = min_ns_.load(std::memory_order_relaxed);
        out.max_ns = max_ns_.load(std::memory_order_relaxed);
        out.mean_ns = static_cast<double>(total_ns_.load(std::memory_order_relaxed)) /
                      static_cast<double>(out.count);
        out.p50_ns = percentileUpperBound(0.50, out.count);
        out.p95_ns = percentileUpperBound(0.95, out.count);
        out.p99_ns = percentileUpperBound(0.99, out.count);
        out.p999_ns = percentileUpperBound(0.999, out.count);
        return out;
    }

private:
    static size_t bucketFor(uint64_t value) noexcept {
        if (value <= 1) return 0;
        const int width = std::bit_width(value - 1);
        return width >= static_cast<int>(kBucketCount) ? kBucketCount - 1
                                                        : static_cast<size_t>(width);
    }

    static uint64_t bucketUpperBound(size_t bucket) noexcept {
        if (bucket >= 63) return std::numeric_limits<uint64_t>::max();
        return uint64_t{1} << bucket;
    }

    uint64_t percentileUpperBound(double percentile, uint64_t count) const noexcept {
        uint64_t rank = static_cast<uint64_t>(
            std::ceil(percentile * static_cast<double>(count)));
        if (rank == 0) rank = 1;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < kBucketCount; ++i) {
            cumulative += buckets_[i].load(std::memory_order_relaxed);
            if (cumulative >= rank) return bucketUpperBound(i);
        }
        return max_ns_.load(std::memory_order_relaxed);
    }

    std::array<std::atomic<uint64_t>, kBucketCount> buckets_{};
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> total_ns_{0};
    std::atomic<uint64_t> min_ns_{std::numeric_limits<uint64_t>::max()};
    std::atomic<uint64_t> max_ns_{0};
};

} // namespace lob
