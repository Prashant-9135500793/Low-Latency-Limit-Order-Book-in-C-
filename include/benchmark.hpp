#pragma once
// benchmark.hpp
// Small shared helpers for the benchmark executables: latency sample
// collection, percentile computation, and result printing. No
// external benchmark framework -- kept intentionally minimal so the
// methodology is fully visible.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lob::bench {

struct LatencyStats {
    uint64_t count = 0;
    double mean_ns = 0;
    uint64_t min_ns = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
};

// Sorts `samples` in place and computes summary statistics.
// Percentile method: nearest-rank on the sorted sample vector
// (index = ceil(p * n) - 1, clamped). Simple, deterministic, and
// standard for latency reporting; no interpolation between samples.
inline LatencyStats computeStats(std::vector<uint64_t>& samples_ns) {
    LatencyStats s;
    if (samples_ns.empty()) return s;
    std::sort(samples_ns.begin(), samples_ns.end());

    const size_t n = samples_ns.size();
    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * static_cast<double>(n));
        if (idx >= n) idx = n - 1;
        return samples_ns[idx];
    };

    s.count = n;
    s.min_ns = samples_ns.front();
    s.max_ns = samples_ns.back();
    s.p50_ns = pct(0.50);
    s.p95_ns = pct(0.95);
    s.p99_ns = pct(0.99);
    s.p999_ns = pct(0.999);

    long double sum = 0;
    for (uint64_t v : samples_ns) sum += static_cast<long double>(v);
    s.mean_ns = static_cast<double>(sum / static_cast<long double>(n));
    return s;
}

inline void printStats(const std::string& label, const LatencyStats& s) {
    std::printf("---- %s (n=%llu) ----\n", label.c_str(),
                static_cast<unsigned long long>(s.count));
    std::printf("  min:   %10llu ns\n", static_cast<unsigned long long>(s.min_ns));
    std::printf("  mean:  %10.1f ns\n", s.mean_ns);
    std::printf("  p50:   %10llu ns\n", static_cast<unsigned long long>(s.p50_ns));
    std::printf("  p95:   %10llu ns\n", static_cast<unsigned long long>(s.p95_ns));
    std::printf("  p99:   %10llu ns\n", static_cast<unsigned long long>(s.p99_ns));
    std::printf("  p99.9: %10llu ns\n", static_cast<unsigned long long>(s.p999_ns));
    std::printf("  max:   %10llu ns\n", static_cast<unsigned long long>(s.max_ns));
}

inline void printThroughput(const std::string& label, uint64_t ops, double seconds) {
    std::printf("---- %s throughput ----\n", label.c_str());
    std::printf("  ops:     %llu\n", static_cast<unsigned long long>(ops));
    std::printf("  time:    %.4f s\n", seconds);
    std::printf("  ops/sec: %.0f\n", seconds > 0 ? static_cast<double>(ops) / seconds : 0.0);
}

} // namespace lob::bench
