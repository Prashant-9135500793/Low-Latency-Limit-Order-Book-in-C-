// Optional libFuzzer target. Configure with Clang and
// -DLOB_BUILD_FUZZERS=ON, then run fuzz_matching_engine CORPUS_DIR.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "matching_engine.hpp"

using namespace lob;

namespace {

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (8U * i);
    return value;
}

uint32_t readU32(const uint8_t* data) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (8U * i);
    return value;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    constexpr size_t kCommandBytes = 24;
    constexpr size_t kMaxCommands = 2048;
    MatchingEngine engine;
    std::vector<Trade> trades;

    const size_t commands = std::min(size / kCommandBytes, kMaxCommands);
    for (size_t i = 0; i < commands; ++i) {
        const uint8_t* command = data + i * kCommandBytes;
        const uint8_t opcode = command[0];
        const uint64_t id = readU64(command + 1);

        if ((opcode & 0x07U) == 0U) {
            (void)engine.cancel(id);
        } else {
            Order order;
            order.id = id;
            order.side = static_cast<Side>(command[9] & 0x03U);
            order.type = static_cast<OrderType>(command[10] & 0x03U);
            order.time_in_force = static_cast<TimeInForce>(command[11] & 0x03U);
            order.flags = static_cast<OrderFlags>(command[12] & 0x07U);
            const uint64_t raw_price = readU64(command + 13);
            order.price = static_cast<int64_t>(raw_price & 0x7FFF'FFFFULL);
            if ((command[9] & 0x80U) != 0U) order.price = -order.price;
            order.quantity = readU32(command + 20);

            trades.clear();
            (void)engine.processOrder(order, trades);
        }

        if (!engine.book().checkInvariants()) std::abort();
    }
    return 0;
}
