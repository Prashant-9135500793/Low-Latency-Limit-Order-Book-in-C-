#pragma once
// journal.hpp
//
// Append-only binary command journal with per-record CRC32 and
// deterministic replay. The format is fixed-width, pointer-free, and
// encoded explicitly in little endian rather than dumping compiler
// structs (which would make padding/endianness part of the file format).

#include <cstddef>
#include <cstdint>
#include <string>

#include "matching_engine.hpp"
#include "order.hpp"

namespace lob {

enum class JournalOpenMode : uint8_t {
    TRUNCATE = 0,
    APPEND = 1,
};

enum class JournalDurability : uint8_t {
    BUFFERED = 0,
    FDATASYNC_EACH_RECORD = 1,
};

struct JournalReplayResult {
    bool ok = false;
    uint64_t records = 0;
    uint64_t new_orders = 0;
    uint64_t cancels = 0;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t trades = 0;
    uint64_t last_ordinal = 0;
    uint64_t error_offset = 0;
    std::string error;
};

class EventJournal {
public:
    EventJournal() = default;
    ~EventJournal();

    EventJournal(const EventJournal&) = delete;
    EventJournal& operator=(const EventJournal&) = delete;
    EventJournal(EventJournal&& other) noexcept;
    EventJournal& operator=(EventJournal&& other) noexcept;

    bool open(const std::string& path, JournalOpenMode mode,
              JournalDurability durability = JournalDurability::BUFFERED);
    void close() noexcept;

    bool appendNewOrder(const Order& order);
    bool appendCancel(uint64_t order_id);
    bool sync();

    bool isOpen() const noexcept { return fd_ >= 0; }
    uint64_t nextOrdinal() const noexcept { return next_ordinal_; }
    const std::string& lastError() const noexcept { return last_error_; }

    static JournalReplayResult replay(const std::string& path, MatchingEngine& engine);

private:
    bool appendRecord(uint8_t record_type, const Order& order);
    void setError(const std::string& message);

    int fd_ = -1;
    uint64_t next_ordinal_ = 1;
    JournalDurability durability_ = JournalDurability::BUFFERED;
    std::string last_error_;
};

} // namespace lob
