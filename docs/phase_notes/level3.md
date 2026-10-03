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

## Level 3: Throughput investigation — root cause identified

Observed: full pipeline throughput ~0.5M msgs/s (59s for 28.7M messages),
despite Level 2's file-based pipeline achieving ~4.5M msgs/s with the
same parse/pool/ring logic.

Method: instrumented producer loop with per-phase timing
(time_recv, time_alloc, time_parse, time_push).

Result:
  time_recv:  51.7s  (87% of total runtime)
  time_alloc:  3.3s
  time_parse:  0.6s
  time_push:   1.6s

Cross-check: timed replay_tool standalone (no engine running):
  "Sent 28734686 messages in 46.123 s (0.6 M msgs/s)"

Conclusion: the bottleneck is NOT the receive/parse/pool/ring pipeline
(that portion alone is healthy — ~5.4s of real work for 28.7M messages,
consistent with Level 2's numbers). The bottleneck is the SENDER:
replay_tool does one sendto() syscall per message, 28.7 million
individual syscalls, no batching — its own standalone send time (46.1s)
accounts for nearly all of the engine's observed time_recv (51.7s).

This is the send-side mirror of the exact problem recvmmsg solved on
the receive side: one-syscall-per-message overhead dominates at this
message volume. The fix is sendmmsg — the symmetric batched-send
syscall, same mmsghdr/iovec structures, used for sending instead of
receiving.

Key lesson: always verify WHICH side of a pipeline is actually the
bottleneck before optimizing — instrumented timing on the receiver
alone would have wrongly suggested the pool allocator or ring buffer
needed more work, when in fact the real constraint was entirely on
the sender, a component we hadn't even been looking at.

## Level 3: sendmmsg experiment — smaller win than expected

Hypothesis: since recvmmsg gave a dramatic improvement by batching
syscalls, sendmmsg should similarly reduce replay_tool's send time.

Result: 46.1s (sendto, one syscall/message) -> 44.6s (sendmmsg, batch=64)
~3% improvement — effectively noise, NOT the dramatic win recvmmsg gave.

Why the asymmetry: recvmmsg's win came from draining an ALREADY-QUEUED
backlog in one syscall — pure syscall-overhead savings on data the
kernel had already finished receiving. Sending still requires genuine
per-packet kernel work regardless of batching (route lookup, header
construction, checksum, copy into skb, enqueue to receiver's buffer)
— this cost exists once per packet whether or not the syscall wrapping
it is batched. Batching only saves the syscall entry/exit overhead,
which is apparently NOT the dominant cost on the send side for this
workload.

Lesson: syscall batching's payoff depends entirely on whether syscall
overhead or per-operation kernel work dominates the cost — this isn't
knowable in advance without measuring both sides, which is exactly
what this experiment did.

Full pipeline, confirmed again with sendmmsg sender + recvmmsg receiver:
Sent 44.6s / Received (time_recv) 50.6s / Total pipeline 57.6s,
0.5M msgs/s, ZERO packet loss maintained throughout all experiments.