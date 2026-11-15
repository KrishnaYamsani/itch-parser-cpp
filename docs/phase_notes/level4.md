## Level 4 baseline: pipeline_bench (no network, no disk)

Setup: Release build, pool=ring=65536, input 28,734,686 messages (BX 2019-07-30),
1 warmup run discarded, 5 measured runs, verification on every run
(produced == consumed == input count; with book: zero anomalies, empty final book).

| Mode      | Median       | Min / Max     | Spread | Per message |
|-----------|--------------|---------------|--------|-------------|
| no-book   | 3.94 M msgs/s | 3.83 / 4.20  | 9.3%   | ~254 ns     |
| with-book | 3.55 M msgs/s | 3.23 / 3.58  | 9.9%   | ~282 ns     |

Findings:
- The order book adds ~28 ns/message; the hand-off dominates (~254 ns with a
  near-empty consumer).
- Run-to-run spread is ~10%, and later runs drift slower (likely thermal), so
  improvements under ~10% cannot be claimed from a single session. Compare
  old/new binaries by alternating runs.
- Earlier Level 1 figures (145-238M msgs/s) are not a valid parse rate: the
  decoded structs were unused. See the --parse-only measurement for the real
  single-thread ceiling.

Single-thread parse-only (no pool, no ring): median 88.25 M msgs/s (~11.3 ns/msg),
spread 0.9%, identical checksum on every run. Pipeline without the book is
~254 ns/msg, so ~95% of the time is hand-off and memory traffic, not parsing.

## Optimization 1: SPSC return ring replaces the shared CAS free list
Change: free slots travel back to the producer through an SPSC ring
(SpscPool) instead of a CAS on one shared free-list head.
Method: alternating runs of both binaries, 3 pairs, median of 3 runs each,
no-book mode, verification on.
Result: 3.90 -> 7.62 M msgs/s (1.95x), ~256 -> ~131 ns/msg.
Every SPSC run (min 7.49) beat every CAS run (max 4.23).
Not established: that cache-line transfer is the mechanism. FIFO slot reuse
and removal of the CAS fence changed at the same time. Confirming needs
perf c2c / cache-miss counters.
Remaining: ~131 ns vs 11 ns for parsing alone, so ~120 ns of hand-off cost
is still unexplained.

### SpscPool vs CAS pool: re-run with full verification (all runs OK)
| Mode      | CAS pool          | SPSC pool         | Speedup |
|-----------|-------------------|-------------------|---------|
| no-book   | 4.09 M (~245 ns)  | 8.87 M (~113 ns)  | 2.17x   |
| with-book | 3.19 M (~314 ns)  | 4.33 M (~231 ns)  | 1.36x   |
Absolute numbers vary between sessions (SPSC no-book: 7.62 M earlier, 8.87 M
now); compare only within an alternating session.
With the hand-off fixed, the book appears to be the slower pipeline stage;
earlier "book adds ~28 ns" was wrong (stages overlap). To be confirmed by
timing book_check single-threaded.

### Standalone consumer cost
book_check (single thread, no hand-off): 106.0 / 105.4 ns/msg
(parse ~11 ns + book ~95 ns), stable, CONSISTENT.
Pipeline with book (SPSC pool): 4.42 M msgs/s (~226 ns), spread 1.0%.
Pipeline no-book (SPSC pool): ~113 ns (same-session pair measured 4.33 M with book).
Added cost of the book in the pipeline (~118 ns) is about its standalone cost
(~95 ns), so the book is not hidden by overlap; the consumer stage appears to
be the bottleneck. To confirm: ring-full / ring-empty counters; then perf
record on book_check.

### Spin counters (SPSC pool, with book): alloc_empty ~850M, pop_empty ~0
Producer is blocked on free slots nearly all the time; the consumer never waits.
Consumer (book + hand-off) is the bottleneck. push_full is 0 by construction
(pool slots = ring capacity - 1). Without the book the stages are about balanced.
Caveat: the first counter version kept shared counters adjacent in one stack
frame (likely false sharing); with-book throughput fell from 4.42 M to ~3.5 M.
Counters moved to thread-locals; results re-measured below.

### Harness fix: thread-local counters (supersedes earlier numbers)
Early pipeline_bench kept `produced`/`consumed` as adjacent shared locals written by
different threads every message (probable false sharing). With thread-local counters:
no-book, alternating, 3 pairs x median of 3 runs: CAS 4.58 M (~218 ns), SPSC 10.39 M
(~96 ns), 2.2x; every SPSC run beat every CAS run. SPSC with-book: 5.50 M (~182 ns),
spread 0.8%. Earlier tables used the old harness; kept for history, do not quote.
Counters, with-book: alloc_empty in the billions, pop_empty ~0 -> producer blocked,
consumer (hand-off + book) is the bottleneck. Without the book, ~96 ns vs 11 ns parse:
~85 ns hand-off remains.
Lesson: instrumentation can perturb the measurement; keep per-thread counters
thread-local and copy out once at the end.

### Fixed-harness with-book results (alternating, 2 pairs, median of 3 runs each)
CAS 4.21 / 5.93 M vs SPSC 5.52 / 7.64 M (1.31x, 1.29x), all verified.
Smaller than the 2.2x no-book speedup because the consumer (book) is the bottleneck.
Both binaries ran ~38% faster in pair 2 with tiny in-run spread: cause unknown
(thread placement on SMT siblings is one untested guess), so absolute numbers
are only comparable within an alternating session.

### perf profile of book_check (RelWithDebInfo, ~80 ns/msg in that session)
Shares of all samples: reduce_order+insert_order 43% (inlined map/hash code, not separable),
hash find 12.4%, malloc family ~9%, rbtree rebalance ~5.7%, apply 3.9%, parse+loop ~7.5%,
file loading ~12% (outside timed loop). Book ~75% of samples; parse ~7.5%.
Consumer ~182 ns/msg with book vs ~96 ns no-book: book ~half of consumer, hand-off the other half.
Next: perf stat IPC; ablation of price levels to split level-map vs order-table cost.

### Ablation of the price levels (book_check, -O2, alternating 3 pairs)
full 75.5 ns/msg vs no-levels 33.8 ns/msg (medians; spreads 4% / 1.5%).
Price-level std::maps account for ~42 ns of the ~64 ns book cost (upper bound:
removing them also removes node alloc/free). Remainder: order hash table ~23 ns
+ apply, parse ~11 ns.
Ceiling: consumer is ~182 ns/msg with book, so even removing all 42 ns gives ~20%.
Change: flat sorted vectors per side (best price last, binary search).

## Optimization 2: flat sorted-vector price levels (replacing std::map)
Verification first: 10M-message checkpoint reproduced the earlier lossless run exactly
(42,994 live orders, 6,349 symbols, all counters, EEM depth table); final book empty,
CONSISTENT, zero anomalies.
book_check (single thread, -O2, alternating 3 pairs): map 75.1 ns/msg -> flat 58.5 ns/msg
(-22%); spreads < 3%.
pipeline_bench_spsc with book (alternating 2 pairs, median of 3 runs, verified):
7.65 / 7.59 M -> 8.64 / 8.69 M msgs/s (+13-14%).
The gain (16.6 ns) is below the 42 ns ablation ceiling: binary search and element shifts
cost something, and some allocation may remain.
Remaining consumer cost: order hash table (~23 ns by subtraction), parse ~11 ns,
hand-off the rest.