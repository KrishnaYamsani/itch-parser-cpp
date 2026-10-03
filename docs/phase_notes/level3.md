## Level 3: UDP packet loss baseline

| Receiver hot-path work          | Sent       | Received   | Loss %  |
|----------------------------------|-----------|------------|---------|
| printf() every message           | 28,734,686 | 21,608,093 | ~24.8%  |
| printf() every 1M messages only  | 28,734,686 | 28,613,055 | ~0.42%  |

## Level 3: UDP packet loss baseline (before recvmmsg)
- Sender: replay_tool, blasts full BX file (28,734,686 messages) via
  sendto(), one syscall per message, as fast as possible, no pacing.
- Receiver: engine_main, one recvfrom() per packet, minimal hot-path
  work (parse length+type only, print every 1M messages).
- Result: 28,613,055 received / 28,734,686 sent = 99.58% delivered,
  ~0.42% loss (121,631 packets dropped).
- Earlier comparison point: with printf() on EVERY message in the hot
  path, loss was ~24.8% — confirms hot-path work directly causes real
  packet loss with UDP (not just slowness), since kernel silently drops
  packets once its receive buffer fills.
- Remaining loss likely due to: one-syscall-per-packet overhead
  (recvfrom) under sustained max-rate bursts, and/or default kernel
  socket receive buffer (SO_RCVBUF) size being a limiting factor.
- Next: recvmmsg batching + possibly increasing SO_RCVBUF, to test
  whether loss can be driven further down, ideally to zero.