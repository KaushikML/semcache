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

## Implementation notes (Phase 6, 2026-10-23)
- **Status: Accepted.** `src/persist/`: `crc32c`, `codec` (bounds-checked entry encoding),
  `log_format` (WAL segment and snapshot framing), `files` (write-all, fdatasync, dir fsync),
  `persistence` (orchestration).
- **The WAL is fed by the store.** `CacheStore` notifies a `MutationObserver` inside every applied
  mutation (writer thread): inserts become INSERT records; evictions, TTL expiry and purges
  become DELETE records keyed by (context, prompt hash); a replacement is just the new INSERT.
  Replay therefore needs no purge logic and is idempotent by key.
- **Snapshots are exactly point-in-time, not "per-partition consistent".** Entries are immutable
  `shared_ptr`s, so the writer copies the pointer list at LSN L and starts a new segment at L+1;
  a background thread serializes the copy without any lock. The writer pauses only for the
  pointer copy. This also removes the need for a separate maintenance thread.
- **Recovery:** snapshot (all-or-nothing; a corrupt one is renamed `.corrupt-<ts>`), then segments
  with records > L. A torn tail in the last segment is truncated at the first bad CRC. Damage in an
  earlier segment stops replay and sets the rest aside: replaying past a lost DELETE could
  resurrect purged entries, so less cache is the safe failure.
- **Model fingerprint** (SHA-256 of the model file) in every header: vectors written by another
  model are dropped on load and the entries stay as exact-match only.
- **Disk errors** disable persistence (memory-only, `semcache_persistence_enabled 0`) and remove
  the files best-effort, so a crash cannot bring back entries purged while disabled. A retry every
  `retry_interval_ms` reopens a segment and writes a full snapshot.
- **HNSW graphs are still not persisted:** recovery re-inserts vectors and the index is rebuilt
  (measured in bench/results/phase6).
- **Purge** (`POST /admin/purge`, admin key from `SEMCACHE_ADMIN_KEY`) runs on the writer and
  answers after it is applied. It also drops, for `cache.purge_guard_window_s`, inserts from
  requests that started before the purge and match it, so an answer in flight during a purge
  cannot reappear.
- Mutation-tested: disabling the purge guard fails a unit and an integration test; not logging
  removals makes purges come back after a restart (unit + integration tests fail).
