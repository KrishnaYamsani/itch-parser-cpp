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

namespace {

constexpr size_t kPoolCapacity = 65536;
constexpr size_t kRingCapacity = 65536;

using Clock = std::chrono::steady_clock;
using Pool = mie::PoolAllocator<mie::ParsedMessage, kPoolCapacity>;
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

    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire)) {}
        size_t offset = 0;
        while (offset + 2 <= buf.size()) {
            const uint16_t len = mie::read_be16(&buf[offset]);
            if (offset + 2 + len > buf.size()) break;

            auto* p = pool->allocate();
            while (!p) p = pool->allocate();

            [[maybe_unused]] uint8_t type = 0;
            if (mie::parse_packet(&buf[offset], 2u + len, *p, type)) {
                while (!ring->push(p)) {}
                ++produced;
            } else {
                pool->deallocate(p);
            }
            offset += 2u + len;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (!go.load(std::memory_order_acquire)) {}
        while (true) {
            auto v = ring->pop();
            if (!v) {
                if (!producer_done.load(std::memory_order_acquire)) continue;
                v = ring->pop();          // one last try, same shutdown pattern as the engine
                if (!v) break;
            }
            mie::ParsedMessage* msg = *v;
            if (use_book) market->apply(*msg);
            pool->deallocate(msg);        // only after we are done reading msg
            ++consumed;
        }
    });

    const auto t0 = Clock::now();
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    const auto t1 = Clock::now();

    RunResult r;
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.ok = (produced == expected) && (consumed == expected);
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

    std::printf("pipeline_bench: pool=%zu ring=%zu mode=%s runs=%d (+1 warmup)\n",
                kPoolCapacity, kRingCapacity, use_book ? "with-book" : "no-book", runs);
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
        std::printf("run %d: %7.3f s  %7.2f M msgs/s  %s\n", i, r.seconds, rate, r.ok ? "OK" : "VERIFY FAILED");
    }

    const double med = median(rates);
    const double lo = *std::min_element(rates.begin(), rates.end());
    const double hi = *std::max_element(rates.begin(), rates.end());
    std::printf("\nmedian %.2f M msgs/s   min %.2f   max %.2f   spread %.1f%%\n",
                med, lo, hi, 100.0 * (hi - lo) / med);
    return all_ok ? 0 : 2;
}