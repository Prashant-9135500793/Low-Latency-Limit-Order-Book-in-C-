#include "timestamp.hpp"

#include <thread>

namespace lob {

// A tiny busy-wait used by benchmarks to let the CPU frequency/branch
// predictor "warm up" before the timed region starts, without relying
// on sleep (which yields the CPU and can trigger a frequency
// down-clock right before the measurement). Deliberately trivial and
// kept out of the header so it is not inlined away entirely, giving
// benchmarks a real (if small) callable warmup step.
void busy_warmup(uint64_t iterations) noexcept {
    volatile uint64_t sink = 0;
    for (uint64_t i = 0; i < iterations; ++i) {
        sink += i;
    }
    (void)sink;
}

} // namespace lob
