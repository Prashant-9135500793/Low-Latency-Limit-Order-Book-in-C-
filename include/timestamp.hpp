#pragma once
// timestamp.hpp
// Thin wrapper around a monotonic clock for latency measurement.
// We use std::chrono::steady_clock (monotonic, not affected by NTP jumps),
// converted to nanoseconds since an arbitrary epoch. Good enough for
// intra-machine relative latency measurement; NOT wall-clock time.

#include <chrono>
#include <cstdint>

namespace lob {

using clock_t = std::chrono::steady_clock;

// Returns nanoseconds since clock epoch (arbitrary reference point).
inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock_t::now().time_since_epoch())
            .count());
}

// Defined in timestamp.cpp; simple busy-loop used to warm up the CPU
// before a timed benchmark region begins.
void busy_warmup(uint64_t iterations) noexcept;

} // namespace lob
