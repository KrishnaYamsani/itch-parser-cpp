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