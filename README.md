# Market Ingest Engine

A multi-threaded C++20 ingestion engine that replays NASDAQ ITCH 5.0 data over UDP
and processes it through a lock-free pipeline into a verified order book.

**Status:** working end to end; optimization and write-up in progress.

- UDP ingest with `recvmmsg` batching, zero packet loss on 28.7M real messages
- Lock-free pool allocator and cache-line-padded SPSC ring buffer
- Order book verified offline: 28.7M messages, zero anomalies, book ends empty
- Batched async disk logging with `io_uring`

Phase-by-phase notes and measurements are in `docs/phase_notes/`.

## Test data
Sample ITCH 5.0 data not included (too large for git). Download with:
wget "https://emi.nasdaq.com/ITCH/Nasdaq%20BX%20ITCH/20190730.BX_ITCH_50.gz" -P market-data/raw/
gunzip -k market-data/raw/20190730.BX_ITCH_50.gz
