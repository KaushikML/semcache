# ADR-005: Threading model — Asio I/O pool + embed pool + single writer

- Status: Accepted
- Date: 2026-10-18

## Context
The work is a mix of high-concurrency network I/O (hundreds of open upstream streams), CPU-bound
embedding (~5 ms each), latency-critical reads (lookups), and comparatively rare writes (inserts,
evictions, purges).

## Options
1. Thread-per-request with synchronous Beast. Simple, but hundreds of long-lived upstream streams
   mean hundreds of threads.
2. One `io_context` on N threads doing everything, including ORT. CPU work blocks I/O and inflates
   tail latency for unrelated connections.
3. **Async I/O pool + dedicated bounded embed pool + single writer thread + maintenance thread.**
4. Coroutines (`asio::awaitable`) on top of option 3's thread layout.

## Decision
Option 3, written with C++20 coroutines (`asio::awaitable`) for the request flow (option 4 is a
style choice within option 3). Lookups run on the embed thread right after embedding, under a
shared lock. All mutations go through one writer thread via a bounded queue. Details and the lock
ordering are in design.md §7.

## Tradeoffs / consequences
- + I/O threads never block, so CPU-heavy embeds can't stall unrelated streams.
- + Single writer gives a total mutation order: the WAL is trivially consistent and there is no
  writer-writer contention.
- + A bounded embed queue is a natural overload signal → fail open.
- − Inserts become visible asynchronously (typically sub-millisecond), which allows one extra
  upstream call in a race.
- − Cross-executor hops add a few µs. That is negligible against a 2 ms lookup budget.
- If HNSW inserts holding the partition write lock hurt read p99 (measured in Phase 5), the
  options are finer-grained HNSW locking or a copy-on-write index swap. Deferred until measured.
