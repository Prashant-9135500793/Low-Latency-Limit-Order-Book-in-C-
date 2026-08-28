// benchmark_ipc.cpp
//
// Measures REAL cross-process latency and throughput over the mmap
// shared-memory SPSC queues, using fork() to create two independent
// processes that each mmap the same backing file (getting, in
// general, different virtual addresses for the mapping -- exactly
// the scenario the rest of this project is built around).
//
// Two experiments:
//   1. Throughput: parent pushes N uint64 messages as fast as
//      possible into queue A; child drains them and reports elapsed
//      time once it has received all N (parent then reads the result
//      back over a plain pipe, since that's simplest for a two-line
//      "how long did it take" and does not affect what we're timing).
//   2. Latency: ping-pong round trip across the process boundary,
//      analogous to benchmark_spsc's in-process version, so the two
//      numbers are directly comparable.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

#include "benchmark.hpp"
#include "shared_memory.hpp"
#include "spsc_queue.hpp"
#include "timestamp.hpp"

using namespace lob;

namespace {

struct IpcLayout {
    std::atomic<uint32_t> ready{0};
    std::atomic<uint32_t> child_done{0};
    SPSCQueue<uint64_t, 1u << 14> a_to_b; // parent -> child
    SPSCQueue<uint64_t, 1u << 14> b_to_a; // child -> parent
};

constexpr const char* kPath = "/tmp/lob_bench_ipc.dat";

void runThroughput(uint64_t n) {
    SharedMemoryRegion::remove_file(kPath);
    SharedMemoryRegion region;
    if (!region.create(kPath, sizeof(IpcLayout))) {
        std::fprintf(stderr, "failed to create ipc bench region\n");
        return;
    }
    auto* layout = new (region.data()) IpcLayout();

    int pipefd[2];
    if (pipe(pipefd) != 0) { std::perror("pipe"); return; }

    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return; }

    if (pid == 0) {
        // Child: consumer. Opens its own mapping of the same file
        // (independent mmap() call -> possibly a different virtual
        // address than the parent's mapping).
        close(pipefd[0]);
        SharedMemoryRegion child_region;
        if (!child_region.open(kPath, sizeof(IpcLayout))) _exit(1);
        auto* child_layout = reinterpret_cast<IpcLayout*>(child_region.data());
        while (!child_layout->ready.load(std::memory_order_acquire)) {}

        uint64_t received = 0;
        uint64_t v;
        auto t0 = std::chrono::steady_clock::now();
        while (received < n) {
            if (child_layout->a_to_b.try_pop(v)) ++received;
        }
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        char buf[64];
        int len = std::snprintf(buf, sizeof(buf), "%.9f", secs);
        ssize_t written = write(pipefd[1], buf, static_cast<size_t>(len));
        (void)written;
        close(pipefd[1]);
        child_layout->child_done.store(1, std::memory_order_release);
        _exit(0);
    }

    // Parent: producer.
    close(pipefd[1]);
    layout->ready.store(1, std::memory_order_release);
    for (uint64_t i = 0; i < n; ++i) {
        while (!layout->a_to_b.try_push(i)) std::this_thread::yield();
    }

    char buf[64] = {};
    ssize_t r = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    double child_secs = (r > 0) ? std::atof(buf) : -1.0;

    int status = 0;
    waitpid(pid, &status, 0);
    region.close();
    SharedMemoryRegion::remove_file(kPath);

    if (child_secs > 0) {
        bench::printThroughput("cross-process mmap SPSC (n=" + std::to_string(n) + ")", n,
                                child_secs);
    } else {
        std::fprintf(stderr, "child reported no timing (pipe read failed)\n");
    }
}

void runLatency(uint64_t n) {
    SharedMemoryRegion::remove_file(kPath);
    SharedMemoryRegion region;
    if (!region.create(kPath, sizeof(IpcLayout))) {
        std::fprintf(stderr, "failed to create ipc bench region\n");
        return;
    }
    auto* layout = new (region.data()) IpcLayout();

    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return; }

    if (pid == 0) {
        // Child: responder. Echoes every value it reads on a_to_b
        // straight back on b_to_a.
        SharedMemoryRegion child_region;
        if (!child_region.open(kPath, sizeof(IpcLayout))) _exit(1);
        auto* child_layout = reinterpret_cast<IpcLayout*>(child_region.data());
        while (!child_layout->ready.load(std::memory_order_acquire)) {}

        uint64_t handled = 0;
        uint64_t v;
        while (handled < n) {
            if (child_layout->a_to_b.try_pop(v)) {
                while (!child_layout->b_to_a.try_push(v)) std::this_thread::yield();
                ++handled;
            }
        }
        _exit(0);
    }

    // Parent: initiator.
    layout->ready.store(1, std::memory_order_release);
    busy_warmup(1'000'000);

    std::vector<uint64_t> rtt;
    rtt.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t t0 = now_ns();
        while (!layout->a_to_b.try_push(t0)) std::this_thread::yield();
        uint64_t echoed;
        while (!layout->b_to_a.try_pop(echoed)) std::this_thread::yield();
        const uint64_t t1 = now_ns();
        rtt.push_back(t1 - t0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    region.close();
    SharedMemoryRegion::remove_file(kPath);

    auto stats = bench::computeStats(rtt);
    bench::printStats("cross_process_ping_pong_rtt (n=" + std::to_string(n) + ")", stats);
    std::printf("  (one-way latency is approximately rtt / 2)\n");
}

} // namespace

int main() {
    std::printf("### Cross-process mmap SPSC throughput ###\n");
    for (uint64_t n : {1'000'000ull, 5'000'000ull, 10'000'000ull}) {
        runThroughput(n);
    }

    std::printf("\n### Cross-process mmap SPSC round-trip latency ###\n");
    // See the equivalent note in benchmark_spsc.cpp: strict-alternation
    // ping-pong is sensitive to scheduler throttling on CPU-quota-limited
    // hosts, so we use a modest iteration count for reliability here.
    runLatency(3000);

    return 0;
}
