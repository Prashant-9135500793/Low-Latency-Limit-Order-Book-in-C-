#include "journal.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lob {
namespace {

constexpr size_t kRecordSize = 48;
constexpr size_t kChecksumOffset = 44;
constexpr uint32_t kRecordMagic = 0x324A424Cu; // bytes: L B J 2
constexpr uint16_t kRecordVersion = 1;
constexpr uint8_t kNewOrderRecord = 1;
constexpr uint8_t kCancelRecord = 2;

void putU16(std::array<uint8_t, kRecordSize>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void putU32(std::array<uint8_t, kRecordSize>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void putU64(std::array<uint8_t, kRecordSize>& bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint16_t getU16(const std::array<uint8_t, kRecordSize>& bytes, size_t offset) {
    const uint32_t value = static_cast<uint32_t>(bytes[offset]) |
                           (static_cast<uint32_t>(bytes[offset + 1]) << 8U);
    return static_cast<uint16_t>(value);
}

uint32_t getU32(const std::array<uint8_t, kRecordSize>& bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

uint64_t getU64(const std::array<uint8_t, kRecordSize>& bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::string errnoMessage(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

bool writeAll(int fd, const uint8_t* data, size_t size, std::string& error) {
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = errnoMessage("write journal");
            return false;
        }
        if (n == 0) {
            error = "write journal: zero-byte write";
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

enum class ReadResult { FULL, EOF_REACHED, PARTIAL, ERROR };

ReadResult readRecord(int fd, std::array<uint8_t, kRecordSize>& bytes, std::string& error) {
    size_t read_bytes = 0;
    while (read_bytes < bytes.size()) {
        const ssize_t n = ::read(fd, bytes.data() + read_bytes, bytes.size() - read_bytes);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = errnoMessage("read journal");
            return ReadResult::ERROR;
        }
        if (n == 0) {
            if (read_bytes == 0) return ReadResult::EOF_REACHED;
            return ReadResult::PARTIAL;
        }
        read_bytes += static_cast<size_t>(n);
    }
    return ReadResult::FULL;
}

bool validateRecord(const std::array<uint8_t, kRecordSize>& bytes, uint64_t expected_ordinal,
                    std::string& error) {
    if (getU32(bytes, 0) != kRecordMagic) {
        error = "journal record magic mismatch";
        return false;
    }
    if (getU16(bytes, 4) != kRecordVersion) {
        error = "unsupported journal record version";
        return false;
    }
    const uint8_t type = bytes[6];
    if (type != kNewOrderRecord && type != kCancelRecord) {
        error = "unknown journal record type";
        return false;
    }
    if (getU64(bytes, 8) != expected_ordinal) {
        error = "journal ordinal gap or reordering";
        return false;
    }
    if (getU32(bytes, kChecksumOffset) != crc32(bytes.data(), kChecksumOffset)) {
        error = "journal CRC32 mismatch";
        return false;
    }
    return true;
}

struct ScanResult {
    bool ok = false;
    uint64_t records = 0;
    uint64_t error_offset = 0;
    std::string error;
};

ScanResult scanFile(const std::string& path) {
    ScanResult result;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            result.ok = true;
            return result;
        }
        result.error = errnoMessage("open journal for scan");
        return result;
    }

    std::array<uint8_t, kRecordSize> bytes{};
    uint64_t expected = 1;
    while (true) {
        std::string read_error;
        const ReadResult read_result = readRecord(fd, bytes, read_error);
        if (read_result == ReadResult::EOF_REACHED) {
            result.ok = true;
            break;
        }
        if (read_result == ReadResult::PARTIAL) {
            result.error = "truncated journal record";
            result.error_offset = result.records * kRecordSize;
            break;
        }
        if (read_result == ReadResult::ERROR) {
            result.error = read_error;
            result.error_offset = result.records * kRecordSize;
            break;
        }
        if (!validateRecord(bytes, expected, result.error)) {
            result.error_offset = result.records * kRecordSize;
            break;
        }
        ++result.records;
        ++expected;
    }
    ::close(fd);
    return result;
}

Order decodeOrder(const std::array<uint8_t, kRecordSize>& bytes) {
    Order order;
    order.id = getU64(bytes, 16);
    order.side = static_cast<Side>(bytes[24]);
    order.type = static_cast<OrderType>(bytes[25]);
    order.time_in_force = static_cast<TimeInForce>(bytes[26]);
    order.flags = static_cast<OrderFlags>(bytes[27]);
    order.price = std::bit_cast<int64_t>(getU64(bytes, 28));
    order.quantity = getU32(bytes, 36);
    order.sequence = 0;
    return order;
}

} // namespace

EventJournal::~EventJournal() { close(); }

EventJournal::EventJournal(EventJournal&& other) noexcept
    : fd_(other.fd_), next_ordinal_(other.next_ordinal_), durability_(other.durability_),
      last_error_(std::move(other.last_error_)) {
    other.fd_ = -1;
    other.next_ordinal_ = 1;
}

EventJournal& EventJournal::operator=(EventJournal&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        next_ordinal_ = other.next_ordinal_;
        durability_ = other.durability_;
        last_error_ = std::move(other.last_error_);
        other.fd_ = -1;
        other.next_ordinal_ = 1;
    }
    return *this;
}

void EventJournal::setError(const std::string& message) { last_error_ = message; }

bool EventJournal::open(const std::string& path, JournalOpenMode mode,
                        JournalDurability durability) {
    close();
    last_error_.clear();
    durability_ = durability;
    next_ordinal_ = 1;

    if (mode == JournalOpenMode::APPEND) {
        const ScanResult scan = scanFile(path);
        if (!scan.ok) {
            setError(scan.error + " at byte offset " + std::to_string(scan.error_offset));
            return false;
        }
        next_ordinal_ = scan.records + 1;
    }

    int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    if (mode == JournalOpenMode::TRUNCATE) flags |= O_TRUNC;
    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0) {
        setError(errnoMessage("open journal"));
        return false;
    }
    return true;
}

void EventJournal::close() noexcept {
    if (fd_ >= 0) {
        // close() flushes kernel buffers to the kernel, while callers
        // needing crash durability can select FDATASYNC_EACH_RECORD or
        // call sync() explicitly before shutdown.
        ::close(fd_);
        fd_ = -1;
    }
}

bool EventJournal::appendNewOrder(const Order& order) {
    return appendRecord(kNewOrderRecord, order);
}

bool EventJournal::appendCancel(uint64_t order_id) {
    Order order;
    order.id = order_id;
    return appendRecord(kCancelRecord, order);
}

bool EventJournal::appendRecord(uint8_t record_type, const Order& order) {
    if (fd_ < 0) {
        setError("journal is not open");
        return false;
    }

    std::array<uint8_t, kRecordSize> bytes{};
    putU32(bytes, 0, kRecordMagic);
    putU16(bytes, 4, kRecordVersion);
    bytes[6] = record_type;
    putU64(bytes, 8, next_ordinal_);
    putU64(bytes, 16, order.id);
    if (record_type == kNewOrderRecord) {
        bytes[24] = static_cast<uint8_t>(order.side);
        bytes[25] = static_cast<uint8_t>(order.type);
        bytes[26] = static_cast<uint8_t>(order.time_in_force);
        bytes[27] = static_cast<uint8_t>(order.flags);
        putU64(bytes, 28, std::bit_cast<uint64_t>(order.price));
        putU32(bytes, 36, order.quantity);
    }
    putU32(bytes, kChecksumOffset, crc32(bytes.data(), kChecksumOffset));

    if (!writeAll(fd_, bytes.data(), bytes.size(), last_error_)) return false;
    ++next_ordinal_;

    if (durability_ == JournalDurability::FDATASYNC_EACH_RECORD && !sync()) return false;
    return true;
}

bool EventJournal::sync() {
    if (fd_ < 0) {
        setError("journal is not open");
        return false;
    }
#if defined(__APPLE__)
    // macOS's libc has no fdatasync(). fsync() alone does not guarantee the
    // drive's write cache is flushed; F_FULLFSYNC is the Darwin-specific
    // call that gives the equivalent durability guarantee.
    if (::fcntl(fd_, F_FULLFSYNC) != 0) {
        setError(errnoMessage("F_FULLFSYNC journal"));
        return false;
    }
#elif defined(__linux__)
    if (::fdatasync(fd_) != 0) {
        setError(errnoMessage("fdatasync journal"));
        return false;
    }
#else
    if (::fsync(fd_) != 0) {
        setError(errnoMessage("fsync journal"));
        return false;
    }
#endif
    return true;
}

JournalReplayResult EventJournal::replay(const std::string& path, MatchingEngine& engine) {
    JournalReplayResult result;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        result.error = errnoMessage("open journal for replay");
        return result;
    }

    std::array<uint8_t, kRecordSize> bytes{};
    std::vector<Trade> trades;
    trades.reserve(8);
    uint64_t expected = 1;

    while (true) {
        std::string read_error;
        const ReadResult read_result = readRecord(fd, bytes, read_error);
        if (read_result == ReadResult::EOF_REACHED) {
            result.ok = true;
            break;
        }
        if (read_result == ReadResult::PARTIAL) {
            result.error = "truncated journal record";
            result.error_offset = result.records * kRecordSize;
            break;
        }
        if (read_result == ReadResult::ERROR) {
            result.error = read_error;
            result.error_offset = result.records * kRecordSize;
            break;
        }
        if (!validateRecord(bytes, expected, result.error)) {
            result.error_offset = result.records * kRecordSize;
            break;
        }

        trades.clear();
        const uint8_t type = bytes[6];
        ExecutionReport report;
        if (type == kNewOrderRecord) {
            ++result.new_orders;
            report = engine.processOrder(decodeOrder(bytes), trades);
        } else {
            ++result.cancels;
            report = engine.cancel(getU64(bytes, 16));
        }
        if (report.accepted()) ++result.accepted;
        else ++result.rejected;
        result.trades += trades.size();
        result.last_ordinal = expected;
        ++result.records;
        ++expected;
    }

    ::close(fd);
    return result;
}

} // namespace lob
