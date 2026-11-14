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