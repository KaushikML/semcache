# Phase 6 — persistence cost

Reproduce: `cmake --build build/release -j4 && ./build/release/semcache_persist_eval --entries 20000 --partitions 200`
(and `--partitions 1`). Raw output with machine details: `persist_20261023T181110Z.txt`.

**Setup:** 20,000 entries, 384-d embedding + 2 KB response each (design §5.1), written through
the real writer thread with the WAL on; then recovery from the WAL alone, a snapshot, and
recovery from the snapshot. Dev laptop (i7-1165G7, WSL2 ext4 virtual disk), RelWithDebInfo.

| | 200 partitions × 100 (brute force) | 1 partition × 20,000 (HNSW) |
|---|---:|---:|
| Inserts through the writer (WAL on) | 8,237 /s | **1,022 /s** |
| WAL size | 73.6 MB | 73.6 MB |
| Recover from WAL | 0.26 s | **17.1 s** |
| Write snapshot | 1.58 s | 1.21 s |
| Snapshot size | 73.4 MB | 73.4 MB |
| Recover from snapshot | 0.25 s | **17.7 s** |

**Takeaways**
- Disk I/O is cheap: ~3.7 KB per entry on disk, 20K entries load in a quarter of a second.
- **HNSW insertion dominates** both recovery and live insert throughput in a large partition:
  ~0.85 ms per vector (M=16, ef_construction=200) on this laptop. Recovery of one 20K-entry
  partition takes ~17 s because the graph is rebuilt from vectors (ADR-008: graphs not persisted),
  and the writer sustains only ~1K inserts/s into such a partition.
- Levers, if the target hardware shows the same (not implemented; decision pending):
  1. recover in the background and serve passthrough meanwhile (readiness reports "loading");
  2. persist the HNSW graph (links) in the snapshot;
  3. lower `index.hnsw_ef_construction` (cheaper inserts, somewhat lower recall).
