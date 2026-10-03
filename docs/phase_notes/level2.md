## Level 2: Pool Allocator + SPSC Ring Buffer
- Pool allocator: fixed-size, intrusive free list, verified single-threaded
  (allocate/deallocate/reuse/exhaustion all behave correctly)
- SPSC ring buffer: cache-line padded head/tail, acquire/release memory
  ordering, verified single-threaded (indexing/wraparound/full/empty logic)
  AND under real concurrency (1M items pushed/popped across two std::thread,
  13 consecutive clean runs, strict ordering maintained, zero corruption)

## Level 2: End-to-end pipeline baseline (producer→pool→ring→consumer)
- Full BX file, 28,734,686 messages, real concurrent parse+allocate+push+pop+deallocate
- Correctness: total parsed == total consumed, exact match, verified
- Throughput: 4.5M msgs/s  (vs. ~150-200M msgs/s single-threaded parse-only, Level 1)
- Known contributors to the regression (to address in Level 4):
  - CAS contention on pool's free_list_head_ (every message does 1 alloc + 1 dealloc)
  - Pure spin-wait on both pool and ring exhaustion (no backoff)
  - Pool/ring capacity (1024) likely too small for this throughput mismatch
  - No thread/core pinning — producer/consumer may be contending for cores