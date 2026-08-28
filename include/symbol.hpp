#pragma once
// symbol.hpp
//
// Fixed-width instrument symbol ("AAPL", "MSFT", ...) plus a
// SymbolOrder wrapper that pairs a Symbol with an Order. The base
// Order type intentionally carries no symbol field (see order.hpp) --
// in the single-book programs (matching_engine_main, feed_handler)
// there is only ever one instrument, so the symbol is implicit. Once
// several instruments and several worker threads are involved, orders
// need to say which book they belong to so a dispatcher can route
// them; SymbolOrder is that routing envelope.

#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>

#include "order.hpp"

namespace lob {

struct Symbol {
    static constexpr size_t kMaxLen = 8;
    // One extra byte keeps c_str() safe even for an exactly 8-character
    // instrument code while preserving a fixed, pointer-free layout.
    char data[kMaxLen + 1] = {};

    Symbol() = default;
    Symbol(const char* s) { // NOLINT(google-explicit-constructor)
        if (s != nullptr) std::strncpy(data, s, kMaxLen);
        data[kMaxLen] = '\0';
    }

    bool operator==(const Symbol& other) const {
        return std::memcmp(data, other.data, kMaxLen) == 0;
    }

    const char* c_str() const { return data; }
};

static_assert(std::is_trivially_copyable_v<Symbol>,
              "Symbol must be trivially copyable (queue/shared-memory friendly)");
static_assert(std::is_standard_layout_v<Symbol>);

// Order tagged with which instrument it belongs to -- the unit of
// work pushed through the MPMC ingestion queue and routed to a shard.
struct SymbolOrder {
    Symbol symbol;
    Order order;
};
static_assert(std::is_trivially_copyable_v<SymbolOrder>);
static_assert(std::is_standard_layout_v<SymbolOrder>);

} // namespace lob

namespace std {
template <>
struct hash<lob::Symbol> {
    size_t operator()(const lob::Symbol& s) const noexcept {
        // FNV-1a over the fixed-width buffer -- good enough dispersion
        // for a handful of ticker symbols mapping to a handful of
        // shards; not a cryptographic hash and not meant to be one.
        size_t h = 1469598103934665603ull;
        for (size_t i = 0; i < lob::Symbol::kMaxLen; ++i) {
            h ^= static_cast<unsigned char>(s.data[i]);
            h *= 1099511628211ull;
        }
        return h;
    }
};
} // namespace std
