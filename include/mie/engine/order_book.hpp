#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "mie/engine/parsed_message.hpp"

namespace mie {

// Price-level book for ONE symbol: price -> total resting shares.
class OrderBook {
public:
    using Bids = std::map<uint32_t, uint64_t, std::greater<uint32_t>>;  // best (highest) first
    using Asks = std::map<uint32_t, uint64_t>;                          // best (lowest) first

    OrderBook() { symbol_.fill(' '); }

    void set_symbol(const std::array<char, 8>& s) { symbol_ = s; }

    void add(char side, uint32_t price, uint32_t shares) {
        if (side == 'B') bids_[price] += shares;
        else             asks_[price] += shares;
    }

    // Returns false if the level doesn't hold at least `shares`
    // (meaning the order table and the book disagree).
    bool reduce(char side, uint32_t price, uint32_t shares) {
        return side == 'B' ? reduce_level(bids_, price, shares)
                           : reduce_level(asks_, price, shares);
    }

    const Bids& bids() const { return bids_; }
    const Asks& asks() const { return asks_; }
    size_t levels() const { return bids_.size() + asks_.size(); }

    std::string_view symbol() const {
        std::string_view s(symbol_.data(), symbol_.size());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
        return s;
    }

private:
    template <typename Map>
    static bool reduce_level(Map& m, uint32_t price, uint32_t shares) {
        auto it = m.find(price);
        if (it == m.end() || it->second < shares) return false;
        it->second -= shares;
        if (it->second == 0) m.erase(it);
        return true;
    }

    Bids bids_;
    Asks asks_;
    std::array<char, 8> symbol_{};
};

struct BookStats {
    uint64_t adds = 0, deletes = 0, executes = 0, cancels = 0, replaces = 0;
    uint64_t orphans = 0;       // message referenced an order we never saw
    uint64_t duplicates = 0;    // an add reused a live order reference
    uint64_t inconsistent = 0;  // tried to remove more shares than the order/level held
};

// All books plus the order table. Owned and mutated by ONE thread (the consumer).
class MarketState {
    struct LiveOrder { uint16_t locate; char side; uint32_t price; uint32_t shares; };
    using OrderMap = std::unordered_map<uint64_t, LiveOrder>;

public:
    // ITCH gives every symbol a numeric id (stock_locate, 16 bits) for the day,
    // so books live in a flat vector indexed by it: no hashing, no string keys.
    static constexpr size_t kMaxSymbols = 65536;

    MarketState() : books_(kMaxSymbols) { orders_.reserve(1u << 20); }

    void apply(const ParsedMessage& msg) {
        switch (msg.kind) {
        case Msgkind::Add: {
            const auto& a = msg.add;
            books_[a.stock_locate].set_symbol(a.stock);
            insert_order(a.order_ref_num, a.stock_locate, a.buy_sell_indicator, a.price, a.shares);
            ++stats_.adds;
            break;
        }
        case Msgkind::Delete: {
            auto it = orders_.find(msg.del.order_ref_num);
            if (it == orders_.end()) { ++stats_.orphans; break; }
            reduce_order(it, it->second.shares);   // argument is read before the entry can be erased
            ++stats_.deletes;
            break;
        }
        case Msgkind::Executed:
            reduce_by_ref(msg.exec.order_ref_num, msg.exec.executed_shares, stats_.executes);
            break;
        case Msgkind::ExecutedWithPrice:
            reduce_by_ref(msg.exec_price.order_ref_num, msg.exec_price.executed_shares, stats_.executes);
            break;
        case Msgkind::Cancel:
            reduce_by_ref(msg.cancel.order_ref_num, msg.cancel.cancelled_shares, stats_.cancels);
            break;
        case Msgkind::Replace: {
            // A replace is "delete the old order, add a new one": new ref, same side and symbol.
            const auto& r = msg.replace;
            auto it = orders_.find(r.original_order_ref_num);
            if (it == orders_.end()) { ++stats_.orphans; break; }
            const LiveOrder old = it->second;      // copy first: reduce_order may erase the entry
            reduce_order(it, old.shares);
            insert_order(r.new_order_ref_num, old.locate, old.side, r.price, r.shares);
            ++stats_.replaces;
            break;
        }
        default:
            break;   // Other: doesn't affect the book
        }
    }

    const BookStats& stats() const { return stats_; }
    size_t live_orders() const { return orders_.size(); }

    // Rebuilds expected level totals from the order table and compares them with
    // the books. Independent of the incremental update logic, so it can catch drift.
    bool verify() const {
        std::map<std::tuple<uint16_t, char, uint32_t>, uint64_t> expected;
        for (const auto& kv : orders_) {
            const LiveOrder& o = kv.second;
            expected[{o.locate, o.side, o.price}] += o.shares;
        }
        size_t levels = 0;
        for (size_t loc = 0; loc < books_.size(); ++loc) {
            for (const auto& lv : books_[loc].bids()) {
                ++levels;
                auto it = expected.find({static_cast<uint16_t>(loc), 'B', lv.first});
                if (it == expected.end() || it->second != lv.second) return false;
            }
            for (const auto& lv : books_[loc].asks()) {
                ++levels;
                auto it = expected.find({static_cast<uint16_t>(loc), 'S', lv.first});
                if (it == expected.end() || it->second != lv.second) return false;
            }
        }
        return levels == expected.size();
    }

    void print_depth(uint16_t locate, int depth) const {
        const OrderBook& b = books_[locate];
        const auto name = b.symbol();
        std::printf("Depth for %.*s (locate %u):\n", static_cast<int>(name.size()), name.data(), locate);
        std::printf("  %-24s | %-24s\n", "BID (price x shares)", "ASK (price x shares)");
        auto bi = b.bids().begin();
        auto ai = b.asks().begin();
        for (int i = 0; i < depth; ++i) {
            char left[48] = "", right[48] = "";
            if (bi != b.bids().end()) {
                std::snprintf(left, sizeof(left), "%.4f x %llu", bi->first / 10000.0,
                              static_cast<unsigned long long>(bi->second));
                ++bi;
            }
            if (ai != b.asks().end()) {
                std::snprintf(right, sizeof(right), "%.4f x %llu", ai->first / 10000.0,
                              static_cast<unsigned long long>(ai->second));
                ++ai;
            }
            std::printf("  %-24s | %-24s\n", left, right);
        }
    }

    void print_report(const char* label, bool run_verify) const {
        size_t active = 0, busiest = 0, busiest_levels = 0;
        for (size_t i = 0; i < books_.size(); ++i) {
            const size_t lv = books_[i].levels();
            if (lv > 0) ++active;
            if (lv > busiest_levels) { busiest_levels = lv; busiest = i; }
        }
        auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        std::printf("\n=== %s ===\n", label);
        std::printf("live orders: %zu, symbols with resting orders: %zu\n", orders_.size(), active);
        std::printf("adds=%llu deletes=%llu executes=%llu cancels=%llu replaces=%llu\n",
                    u(stats_.adds), u(stats_.deletes), u(stats_.executes),
                    u(stats_.cancels), u(stats_.replaces));
        std::printf("orphans=%llu duplicates=%llu inconsistent=%llu\n",
                    u(stats_.orphans), u(stats_.duplicates), u(stats_.inconsistent));
        if (run_verify)
            std::printf("order table vs book levels: %s\n", verify() ? "CONSISTENT" : "MISMATCH");
        if (busiest_levels > 0) print_depth(static_cast<uint16_t>(busiest), 5);
    }

private:
    void insert_order(uint64_t ref, uint16_t locate, char side, uint32_t price, uint32_t shares) {
        auto res = orders_.emplace(ref, LiveOrder{locate, side, price, shares});
        if (!res.second) { ++stats_.duplicates; return; }
        books_[locate].add(side, price, shares);
    }

    // Removes `shares` from an order and its price level; erases the order at zero.
    void reduce_order(OrderMap::iterator it, uint32_t shares) {
        LiveOrder& o = it->second;
        if (shares > o.shares) { ++stats_.inconsistent; shares = o.shares; }
        if (!books_[o.locate].reduce(o.side, o.price, shares)) ++stats_.inconsistent;
        o.shares -= shares;
        if (o.shares == 0) orders_.erase(it);
    }

    void reduce_by_ref(uint64_t ref, uint32_t shares, uint64_t& counter) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++stats_.orphans; return; }
        reduce_order(it, shares);
        ++counter;
    }

    OrderMap orders_;
    std::vector<OrderBook> books_;
    BookStats stats_;
};

} // namespace mie