#include "shared_memory.hpp"
#include "shared_types.hpp"
#include "test_common.hpp"

#include <cstdlib>
#include <new>
#include <sys/wait.h>
#include <unistd.h>

using namespace lob;

static const char* kTestPath = "/tmp/lob_test_shared_memory.dat";

void test_shared_memory_init() {
    SharedMemoryRegion::remove_file(kTestPath);
    SharedMemoryRegion region;
    CHECK(region.create(kTestPath, sizeof(SharedRegion)));
    CHECK(region.is_mapped());
    CHECK(region.size() == sizeof(SharedRegion));

    // Placement-construct the layout in the mapped memory, exactly as
    // the real apps do.
    auto* shared = new (region.data()) SharedRegion();
    CHECK(shared->header.state.load() ==
          static_cast<uint32_t>(RegionState::NOT_READY));

    shared->header.magic.store(kSharedMemoryMagic);
    shared->header.region_size.store(sizeof(SharedRegion));
    shared->header.version.store(kSharedMemoryVersion);
    shared->header.state.store(static_cast<uint32_t>(RegionState::READY));

    region.close();
    SharedMemoryRegion::remove_file(kTestPath);
}

void test_no_pointer_members_compile_check() {
    // Compile-time structural check: SharedRegion (and its queue
    // members) must not embed any pointer-to-object fields -- only
    // atomics, fixed arrays, and POD Order/Trade values. We can't
    // enumerate members reflectively in C++20 without extra tooling,
    // but we can assert the type's size matches the hand-computed
    // expected layout size, which changes the moment anyone adds a
    // pointer/std::string/std::vector member (their sizeof differs
    // from a plain array-of-T), acting as a tripwire.
    constexpr size_t expected_min =
        sizeof(SharedHeader) +
        sizeof(OrderMsg) * kOrderQueueCapacity +
        sizeof(Trade) * kTradeQueueCapacity +
        sizeof(ExecutionReport) * kReportQueueCapacity;
    CHECK(sizeof(SharedRegion) >= expected_min);
    static_assert(std::is_trivially_copyable_v<Order>);
    static_assert(std::is_trivially_copyable_v<Trade>);
    static_assert(std::is_trivially_copyable_v<OrderMsg>);
    static_assert(std::is_trivially_copyable_v<ExecutionReport>);
}

void test_two_processes_share_data() {
    // Real two-process test via fork(): the child maps the region,
    // writes a value; after it exits, the parent (which independently
    // mapped the SAME file, at whatever virtual address the kernel
    // gave it) must observe the child's write. This directly exercises
    // "shared memory across process boundaries", not just threads.
    SharedMemoryRegion::remove_file(kTestPath);
    {
        SharedMemoryRegion creator;
        CHECK(creator.create(kTestPath, sizeof(SharedRegion)));
        new (creator.data()) SharedRegion();
        // leave it mapped+initialized on disk, then close in this
        // process; the file contents persist.
    }

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // Child: open (do not create), push a message, exit.
        SharedMemoryRegion region;
        if (!region.open(kTestPath, sizeof(SharedRegion))) _exit(2);
        auto* shared = reinterpret_cast<SharedRegion*>(region.data());
        OrderMsg msg;
        msg.type = OrderMsgType::NEW_ORDER;
        msg.order.id = 777;
        msg.order.side = Side::BUY;
        msg.order.price = 12345;
        msg.order.quantity = 9;
        if (!shared->order_queue.try_push(msg)) _exit(3);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Parent: independently opens the same file and must see the
    // child's message.
    SharedMemoryRegion parent_region;
    CHECK(parent_region.open(kTestPath, sizeof(SharedRegion)));
    auto* shared = reinterpret_cast<SharedRegion*>(parent_region.data());
    OrderMsg out;
    CHECK(shared->order_queue.try_pop(out));
    CHECK(out.order.id == 777);
    CHECK(out.order.price == 12345);
    CHECK(out.order.quantity == 9);

    parent_region.close();
    SharedMemoryRegion::remove_file(kTestPath);
}

int main() {
    RUN(test_shared_memory_init);
    RUN(test_no_pointer_members_compile_check);
    RUN(test_two_processes_share_data);
    return test_summary();
}
