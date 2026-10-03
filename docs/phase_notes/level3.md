## Level 3: UDP packet loss — full progression

| Configuration                                    | Received    | Loss %  |
|---------------------------------------------------|------------|---------|
| recvfrom, printf() every message                  | 21,608,093 | ~24.8%  |
| recvfrom, minimal hot-path work (print every 1M)   | 28,613,055 | ~0.42%  |
| recvmmsg (batch=64) + SO_RCVBUF=8MB                | 28,734,686 | 0.00%   |

Sent: 28,734,686 (full BX sample file, max-rate, no pacing)

### What actually fixed it
1. Removing per-message printf from the hot path (~24.8% -> ~0.42%):
   confirms expensive synchronous work in the receive path directly
   causes real packet loss under UDP (not just slowness) — the kernel
   silently drops packets once its buffer fills while your code is
   busy doing something else (printing).
2. recvmmsg batching + larger SO_RCVBUF (~0.42% -> 0.00%): batching
   cuts per-packet syscall overhead roughly BATCH_SIZE-fold; the larger
   receive buffer gives the kernel more headroom to absorb bursts while
   draining, together eliminating loss entirely for this workload.

### Secondary lesson: sentinel-based stream termination is itself
unreliable over UDP
- A single "end of stream" sentinel packet can be lost like any other
  UDP packet, causing the receiver to hang forever waiting for a
  signal that already came and went.
- Fix applied: send the sentinel multiple times (burst of 5) AND add a
  timeout-based fallback (recvmmsg with a timeout, exits cleanly after
  N seconds of silence) so the receiver can never hang indefinitely,
  regardless of what gets lost.

## Level 3: Full pipeline (UDP -> parse -> pool -> ring -> consumer)

Correctness: 28,734,686 sent == received == consumed. Exact match, zero
packet loss, confirmed with real network transport (loopback UDP).

Throughput: 0.4M msgs/s (76.2s for full file)
  vs. Level 2 file-based pipeline: ~4.5M msgs/s (~10x faster)
  vs. standalone recvmmsg-only receiver (no parsing/pool/ring): zero loss,
      much faster drain (exact number not captured, but no backlog issues)

Hypothesis: the producer thread is now doing recvmmsg (blocking network
syscall) + full message parsing + CAS-based pool allocation + ring push,
all serialized in one thread, for every message. Each of these was
already a known cost in isolation (Level 2 established CAS contention
as expensive; network I/O has its own latency) — this is the first test
stacking all of them together in one thread's critical path, and the
compounding effect is the likely cause of the ~10x slowdown vs. the
file-based pipeline.

Candidate Level 4 investigations:
- Is recvmmsg's blocking wait actually idle time counted in this 76s,
  or genuine processing cost? (sender's own send rate may also be a
  factor — worth measuring replay_tool's own elapsed time separately)
- Would splitting "receive batch" and "parse batch" into two steps
  (rather than parsing immediately per-packet inside the recv loop)
  change anything, or is the cost genuinely per-message regardless of
  structure?