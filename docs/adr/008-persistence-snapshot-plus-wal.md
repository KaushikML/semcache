# ADR-008: Persistence — periodic snapshot + append-only WAL

- Status: Proposed (implemented in Phase 6)
- Date: 2026-10-18

## Context
The cache should survive restarts (a cold cache costs money), but losing a few seconds of inserts
is harmless. Startup must tolerate corrupted files (fail open).

## Options
1. No persistence.
2. Snapshot only, every N seconds. Loses up to N seconds, and rewrites everything every time.
3. WAL only. Unbounded log, slow recovery.
4. **Snapshot + WAL.** Write the snapshot to a temp file with LSN L, fsync, rename atomically, then
   drop WAL segments ≤ L. Recovery = load snapshot + replay WAL records > L.
5. Embedded KV store (RocksDB/LMDB). Heavy dependency for what is effectively an append log.

## Decision
Option 4. The writer thread appends each mutation (insert / delete / purge) to the WAL in the same
order it applies it. Records are `[len u32][crc32c u32][type u8][lsn u64][payload]`, and fsync
runs every `persistence.fsync_interval_ms`. The snapshot is written by the maintenance thread
under per-partition shared locks. HNSW graphs are **not** persisted in v1: indexes are rebuilt
from vectors on load (measured in Phase 6; persisted graphs only if rebuild time justifies it).

## Tradeoffs / consequences
- + Bounded loss window, fast restart, and simple binary formats that are easy to fuzz.
- + Torn tail → truncate at the first bad CRC. Bad snapshot → move aside and start empty.
- − The snapshot is not perfectly point-in-time across partitions (each partition is consistent,
  and WAL replay > L repairs the rest, because replay is idempotent by entry id).
- − Disk-full disables persistence and the cache becomes memory-only, which is reported via
  metrics.
