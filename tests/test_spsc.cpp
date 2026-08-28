#include "spsc_queue.hpp"
#include "test_common.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace lob;

void test_empty_queue() {
    SPSCQueue<int, 8> q;
    CHECK(q.empty());
    CHECK(!q.full());
    CHECK(q.size() == 0);
    int v;
    CHECK(!q.try_pop(v));
}

void test_full_queue() {
    SPSCQueue<int, 4> q;
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(q.try_push(3));
    CHECK(q.try_push(4));
    CHECK(q.full());
    CHECK(!q.try_push(5)); // must not overwrite unread data
    CHECK(q.size() == 4);
}

void test_push_pop_basic() {
    SPSCQueue<int, 4> q;
    CHECK(q.try_push(42));
    int v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 42);
    CHECK(q.empty());
}

void test_fifo_ordering() {
    SPSCQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) CHECK(q.try_push(i));
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == i); // strict FIFO
    }
}

void test_wraparound() {
    SPSCQueue<int, 4> q;
    // Push/pop repeatedly past the ring boundary many times.
    int next_push = 0;
    int next_pop = 0;
    for (int round = 0; round < 100; ++round) {
        CHECK(q.try_push(next_push++));
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == next_pop++);
    }
}

void test_producer_consumer_threads() {
    // Real cross-thread correctness test: one producer thread pushes
    // N sequential ints, one consumer thread pops them, and we verify
    // no loss, no duplication, and strict ordering.
    constexpr int kCapacity = 1024;
    constexpr int kCount = 2'000'000;
    SPSCQueue<int, kCapacity> q;

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            while (!q.try_push(i)) {
                // spin
            }
        }
    });

    std::vector<int> received;
    received.reserve(kCount);
    std::thread consumer([&] {
        int received_count = 0;
        while (received_count < kCount) {
            int v;
            if (q.try_pop(v)) {
                received.push_back(v);
                ++received_count;
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK(static_cast<int>(received.size()) == kCount);
    bool ordered_and_complete = true;
    for (int i = 0; i < kCount; ++i) {
        if (received[static_cast<size_t>(i)] != i) {
            ordered_and_complete = false;
            break;
        }
    }
    CHECK(ordered_and_complete);
}

int main() {
    RUN(test_empty_queue);
    RUN(test_full_queue);
    RUN(test_push_pop_basic);
    RUN(test_fifo_ordering);
    RUN(test_wraparound);
    RUN(test_producer_consumer_threads);
    return test_summary();
}
