## Level 2: Pool Allocator + SPSC Ring Buffer

### Correctness
- Pool allocator: fixed-size, intrusive free list, made lock-free via
  atomic CAS (Treiber-stack pattern) after an initial plain-pointer
  version caused a hang/data race under real two-thread load.
- SPSC ring buffer: cache-line padded head/tail, acquire/release ordering,
  verified single-threaded and under real concurrency.
- Full pipeline (producer: parse + allocate + push, consumer: pop +
  deallocate) verified correct end-to-end on the full BX sample file:
  28,734,686 messages parsed == 28,734,686 messages consumed, exact match,
  across multiple runs.

### Capacity sweep experiment
Hypothesis going in: pool/ring capacity (1024 slots) was too small,
causing excessive spin-waiting on full/empty conditions, and was a
major contributor to the throughput drop vs. single-threaded parsing.

Swept capacity 1024 -> 262144 (256x), full file, 2-4 runs per value,
averaged:

| Capacity | Avg throughput (M msgs/s) |
|----------|---------------------------|
| 1024     | 4.5                       |
| 4096     | 4.4                       |
| 16384    | 4.25                      |
| 65536    | 4.68                      |
| 262144   | 4.3                       |

Result: flat, no meaningful trend — well within run-to-run noise.
**Hypothesis rejected: capacity is not the bottleneck.**

Settled on Capacity = 65536 (best observed average; no benefit from
going higher, and higher capacity costs more memory for nothing in
return).

### Bug encountered: stack overflow at large capacity
At Capacity=262144, PoolAllocator/SpscRing (declared as local stack
variables) exceeded the default 8MB thread stack size
(sizeof(ParsedMessage) * 262144 ~= 14.7MB), causing a segfault.
Fix: heap-allocate both via std::make_unique instead of declaring as
stack locals. General lesson: large fixed-size buffers belong on the
heap regardless of whether they technically fit on the stack at a
given size — the failure mode (silent segfault, no clear error) is
bad enough to avoid by default.

### Corrected understanding of the throughput regression
Baseline: ~150-210M msgs/s single-threaded parse-only (Level 1)
         vs. ~4.2-4.7M msgs/s full two-thread pipeline (Level 2)
         -- roughly a 30-40x drop.

Originally hypothesized causes (see earlier baseline note): capacity,
spin-waiting, core contention. Capacity is now experimentally ruled
out. Remaining, still-untested hypotheses for Level 4 investigation:
- CAS contention itself: every message triggers one atomic allocate +
  one atomic deallocate, both hitting the SAME cache line
  (free_list_head_) from two different cores, 28.7M times. This is
  likely the dominant cost — unlike capacity, this cost is paid per
  OPERATION, not per buffer-full event, so it doesn't improve with
  a bigger buffer.
- Pure spin-wait (no backoff) burning CPU on both threads.
- No thread/core pinning — producer and consumer may be contending
  for the same physical cores with each other and the OS.
- Candidate Level 4 fixes to test: batch multiple messages per
  pool-allocation (amortize CAS cost), add exponential backoff to
  spin loops, pin producer/consumer to separate physical cores,
  consider a batching/chunked ring buffer design instead of
  one-atomic-op-per-message.