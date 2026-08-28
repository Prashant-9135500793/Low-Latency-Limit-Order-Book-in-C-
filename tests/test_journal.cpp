#include "journal.hpp"
#include "test_common.hpp"

#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace lob;

namespace {

Order makeOrder(uint64_t id, Side side, int64_t price, uint32_t quantity,
                OrderType type = OrderType::LIMIT,
                TimeInForce tif = TimeInForce::GTC,
                OrderFlags flags = OrderFlags::NONE) {
    Order order;
    order.id = id;
    order.side = side;
    order.type = type;
    order.time_in_force = tif;
    order.flags = flags;
    order.price = price;
    order.quantity = quantity;
    return order;
}

std::string tempPath(const char* suffix) {
    return std::string("/tmp/lob_journal_") + std::to_string(::getpid()) + suffix;
}

void compareEngineState(const MatchingEngine& lhs, const MatchingEngine& rhs) {
    CHECK(lhs.sequence() == rhs.sequence());
    CHECK(lhs.tradeCount() == rhs.tradeCount());
    CHECK(lhs.book().orderCount() == rhs.book().orderCount());
    CHECK(lhs.book().bestBid() == rhs.book().bestBid());
    CHECK(lhs.book().bestAsk() == rhs.book().bestAsk());
    CHECK(lhs.book().topLevels(Side::BUY, 10).size() ==
          rhs.book().topLevels(Side::BUY, 10).size());
    CHECK(lhs.book().topLevels(Side::SELL, 10).size() ==
          rhs.book().topLevels(Side::SELL, 10).size());
    CHECK(lhs.book().checkInvariants());
    CHECK(rhs.book().checkInvariants());
}

} // namespace

void test_journal_round_trip_and_append() {
    const std::string path = tempPath("_roundtrip.bin");
    ::unlink(path.c_str());

    EventJournal journal;
    CHECK(journal.open(path, JournalOpenMode::TRUNCATE));
    MatchingEngine original;
    std::vector<Trade> trades;

    const auto applyNew = [&](const Order& order) {
        CHECK(journal.appendNewOrder(order));
        trades.clear();
        return original.processOrder(order, trades);
    };
    const auto applyCancel = [&](uint64_t order_id) {
        CHECK(journal.appendCancel(order_id));
        return original.cancel(order_id);
    };

    CHECK(applyNew(makeOrder(1, Side::SELL, 100, 10)).accepted());
    CHECK(applyNew(makeOrder(2, Side::SELL, 101, 20)).accepted());
    CHECK(applyNew(makeOrder(3, Side::BUY, 0, 25, OrderType::MARKET,
                             TimeInForce::IOC))
              .accepted());
    CHECK(applyCancel(2).status == ExecutionStatus::CANCELED);
    CHECK(applyNew(makeOrder(4, Side::BUY, 99, 7, OrderType::LIMIT,
                             TimeInForce::GTC, OrderFlags::POST_ONLY))
              .accepted());
    CHECK(!applyNew(makeOrder(0, Side::BUY, 100, 1)).accepted());
    CHECK(journal.sync());
    journal.close();

    MatchingEngine replayed;
    const auto result = EventJournal::replay(path, replayed);
    CHECK(result.ok);
    CHECK(result.records == 6);
    CHECK(result.new_orders == 5);
    CHECK(result.cancels == 1);
    CHECK(result.trades == 2);
    compareEngineState(original, replayed);

    CHECK(journal.open(path, JournalOpenMode::APPEND));
    CHECK(journal.nextOrdinal() == 7);
    const Order appended = makeOrder(5, Side::SELL, 105, 3);
    CHECK(journal.appendNewOrder(appended));
    trades.clear();
    original.processOrder(appended, trades);
    CHECK(journal.sync());
    journal.close();

    MatchingEngine replayed_after_append;
    const auto appended_result = EventJournal::replay(path, replayed_after_append);
    CHECK(appended_result.ok);
    CHECK(appended_result.records == 7);
    CHECK(appended_result.last_ordinal == 7);
    compareEngineState(original, replayed_after_append);

    ::unlink(path.c_str());
}

void test_journal_detects_corruption_and_truncation() {
    const std::string source = tempPath("_source.bin");
    const std::string corrupt = tempPath("_corrupt.bin");
    const std::string truncated = tempPath("_truncated.bin");
    ::unlink(source.c_str());
    ::unlink(corrupt.c_str());
    ::unlink(truncated.c_str());

    EventJournal journal;
    CHECK(journal.open(source, JournalOpenMode::TRUNCATE));
    CHECK(journal.appendNewOrder(makeOrder(1, Side::BUY, 100, 10)));
    CHECK(journal.appendCancel(1));
    CHECK(journal.sync());
    journal.close();

    std::filesystem::copy_file(source, corrupt,
                               std::filesystem::copy_options::overwrite_existing);
    int fd = ::open(corrupt.c_str(), O_RDWR);
    CHECK(fd >= 0);
    unsigned char byte = 0;
    CHECK(::pread(fd, &byte, 1, 28) == 1);
    byte ^= 0x5A;
    CHECK(::pwrite(fd, &byte, 1, 28) == 1);
    ::close(fd);

    MatchingEngine engine1;
    const auto corrupt_result = EventJournal::replay(corrupt, engine1);
    CHECK(!corrupt_result.ok);
    CHECK(corrupt_result.error.find("CRC32") != std::string::npos);

    std::filesystem::copy_file(source, truncated,
                               std::filesystem::copy_options::overwrite_existing);
    CHECK(::truncate(truncated.c_str(), 95) == 0); // two records would be 96 bytes
    MatchingEngine engine2;
    const auto truncated_result = EventJournal::replay(truncated, engine2);
    CHECK(!truncated_result.ok);
    CHECK(truncated_result.error.find("truncated") != std::string::npos);

    ::unlink(source.c_str());
    ::unlink(corrupt.c_str());
    ::unlink(truncated.c_str());
}

void test_append_refuses_corrupt_existing_file() {
    const std::string path = tempPath("_bad_append.bin");
    ::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    const char garbage[] = "not-a-valid-journal";
    CHECK(::write(fd, garbage, sizeof(garbage)) == static_cast<ssize_t>(sizeof(garbage)));
    ::close(fd);

    EventJournal journal;
    CHECK(!journal.open(path, JournalOpenMode::APPEND));
    CHECK(!journal.lastError().empty());
    ::unlink(path.c_str());
}

int main() {
    RUN(test_journal_round_trip_and_append);
    RUN(test_journal_detects_corruption_and_truncation);
    RUN(test_append_refuses_corrupt_existing_file);
    return test_summary();
}
