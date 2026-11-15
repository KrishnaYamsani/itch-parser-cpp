// pipeline_bench: parse -> pool -> SPSC ring -> consumer, no network, no disk.
// Usage: pipeline_bench <itch_file> [--no-book] [--runs=N]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mie/core/pool_allocator.hpp"
#include "mie/core/spsc_ring.hpp"
#include "mie/engine/order_book.hpp"
#include "mie/engine/packet_parser.hpp"
#include "mie/engine/parsed_message.hpp"
#include "mie/parser/byte_utils.hpp"

#ifdef MIE_POOL_SPSC
#include "mie/core/spsc_pool.hpp"
#endif

namespace {

constexpr size_t kPoolCapacity = 65536;
constexpr size_t kRingCapacity = 65536;

using Clock = std::chrono::steady_clock;
#ifdef MIE_POOL_SPSC
using Pool = mie::SpscPool<mie::ParsedMessage, kRingCapacity>;
constexpr const char* kPoolName = "spsc-return-ring";
#else
using Pool = mie::PoolAllocator<mie::ParsedMessage, kPoolCapacity>;
constexpr const char* kPoolName = "cas-free-list";
#endif
using Ring = mie::SpscRing<mie::ParsedMessage*, kRingCapacity>;

std::vector<uint8_t> load_file(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::perror("open"); std::exit(1); }
    std::vector<uint8_t> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return buf;
}

size_t count_messages(const std::vector<uint8_t>& buf) {
    size_t n = 0, off = 0;
    while (off + 2 <= buf.size()) {
        const uint16_t len = mie::read_be16(&buf[off]);
        if (off + 2 + len > buf.size()) break;
        off += 2 + len;
        ++n;
    }
    return n;
}

struct RunResult {
    double seconds = 0;
    bool ok = false;
    unsigned long long alloc_empty = 0, push_full = 0, pop_empty = 0;
};

RunResult run_once(const std::vector<uint8_t>& buf, size_t expected, bool use_book) {
    // Everything is built BEFORE the timer starts. Pool and ring are large,
    // so they live on the heap (the stack-overflow lesson from Level 2).
    auto pool = std::make_unique<Pool>();
    auto ring = std::make_unique<Ring>();
    auto market = std::make_unique<mie::MarketState>();

    std::atomic<bool> go{false};
    std::atomic<bool> producer_done{false};
    size_t produced = 0, consumed = 0;
    size_t alloc_empty = 0, push_full = 0, pop_empty = 0;

    std::thread producer([&] {
        size_t my_produced = 0, my_alloc_empty = 0, my_push_full = 0;
        while (!go.load(std::memory_order_acquire)) {}
        size_t offset = 0;
        while (offset + 2 <= buf.size()) {
            const uint16_t len = mie::read_be16(&buf[offset]);
            if (offset + 2 + len > buf.size()) break;

            auto* p = pool->allocate();
            while (!p) { ++my_alloc_empty; p = pool->allocate(); }

            [[maybe_unused]] uint8_t type = 0;
            if (mie::parse_packet(&buf[offset], 2u + len, *p, type)) {
                while (!ring->push(p)) { ++my_push_full; }
                ++my_produced;
            } else {
                pool->deallocate(p);
            }
            offset += 2u + len;
        }
        produced = my_produced; alloc_empty = my_alloc_empty; push_full = my_push_full;   
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        size_t my_consumed = 0, my_pop_empty = 0;
        while (!go.load(std::memory_order_acquire)) {}
        while (true) {
            auto v = ring->pop();
            if (!v) {
                if (!producer_done.load(std::memory_order_acquire)) { ++my_pop_empty; continue; }
                v = ring->pop();
                if (!v) break;
            }
            mie::ParsedMessage* msg = *v;
            if (use_book) market->apply(*msg);
            pool->deallocate(msg);        // only after we are done reading msg
            ++my_consumed;
        }
        consumed = my_consumed; pop_empty = my_pop_empty;  
    });

    const auto t0 = Clock::now();
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    
    const auto t1 = Clock::now();

    RunResult r;
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.ok = (produced == expected) && (consumed == expected);
    r.alloc_empty = alloc_empty; r.push_full = push_full; r.pop_empty = pop_empty;
    if (use_book) {
        const auto& s = market->stats();
        r.ok = r.ok && s.orphans == 0 && s.duplicates == 0 &&
               s.inconsistent == 0 && market->live_orders() == 0;
    }
    return r;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Single thread, no pool, no ring. The checksum depends on decoded fields so
// the compiler cannot eliminate the parsing.
double parse_only_once(const std::vector<uint8_t>& buf, uint64_t& checksum) {
    mie::ParsedMessage msg;
    uint64_t sum = 0;
    size_t off = 0;
    const auto t0 = Clock::now();
    while (off + 2 <= buf.size()) {
        const uint16_t len = mie::read_be16(&buf[off]);
        if (off + 2 + len > buf.size()) break;
        uint8_t type = 0;
        if (mie::parse_packet(&buf[off], 2u + len, msg, type)) {
            sum += static_cast<uint64_t>(msg.kind);
            switch (msg.kind) {
                case mie::Msgkind::Add:      sum += msg.add.order_ref_num + msg.add.price; break;
                case mie::Msgkind::Delete:   sum += msg.del.order_ref_num; break;
                case mie::Msgkind::Executed: sum += msg.exec.order_ref_num + msg.exec.executed_shares; break;
                default: break;
            }
        }
        off += 2u + len;
    }
    checksum = sum;
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch_file> [--no-book] [--runs=N]\n", argv[0]);
        return 1;
    }
    bool use_book = true;
    int runs = 5;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-book") use_book = false;
        else if (a.rfind("--runs=", 0) == 0) runs = std::max(1, std::atoi(a.c_str() + 7));
    }

    const auto buf = load_file(argv[1]);
    const size_t expected = count_messages(buf);

    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--parse-only") {
            std::vector<double> rates;
            uint64_t cs = 0;
            parse_only_once(buf, cs);                       // warmup
            for (int r = 0; r < 5; ++r) {
                const double s = parse_only_once(buf, cs);
                rates.push_back(expected / s / 1e6);
                std::printf("parse-only run %d: %.3f s  %.2f M msgs/s  (checksum %llu)\n",
                    r + 1, s, rates.back(), static_cast<unsigned long long>(cs));
            }
            std::printf("median %.2f M msgs/s\n", median(rates));
            return 0;
        }
    }

    std::printf("pipeline_bench: pool=%s ring=%zu mode=%s runs=%d (+1 warmup)\n",
            kPoolName, kRingCapacity, use_book ? "with-book" : "no-book", runs);
#ifdef __OPTIMIZE__
    std::printf("build: optimized\n");
#else
    std::printf("build: NOT optimized -- numbers are meaningless, rebuild with -DCMAKE_BUILD_TYPE=Release\n");
#endif
    std::printf("input: %zu messages\n", expected);

    const RunResult warm = run_once(buf, expected, use_book);
    std::printf("warmup: %.3f s (discarded)%s\n", warm.seconds, warm.ok ? "" : "  VERIFY FAILED");

    std::vector<double> rates;
    bool all_ok = warm.ok;
    for (int i = 1; i <= runs; ++i) {
        const RunResult r = run_once(buf, expected, use_book);
        const double rate = expected / r.seconds / 1e6;
        rates.push_back(rate);
        all_ok = all_ok && r.ok;
        std::printf("run %d: %7.3f s  %7.2f M msgs/s  %s  alloc_empty=%llu push_full=%llu pop_empty=%llu\n",
                    i, r.seconds, rate, r.ok ? "OK" : "VERIFY FAILED",
                    r.alloc_empty, r.push_full, r.pop_empty);
        }

    const double med = median(rates);
    const double lo = *std::min_element(rates.begin(), rates.end());
    const double hi = *std::max_element(rates.begin(), rates.end());
    std::printf("\nmedian %.2f M msgs/s   min %.2f   max %.2f   spread %.1f%%\n",
                med, lo, hi, 100.0 * (hi - lo) / med);
    return all_ok ? 0 : 2;
}