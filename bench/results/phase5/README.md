# Phase 5 — brute force vs HNSW

Reproduce: `cmake --build build/release -j4 && ./build/release/semcache_hnsw_eval --sizes 1000,5000,10000,20000,50000 --queries 500 --out results.json`.
Raw data: `hnsw_20261021T154004Z.json`; machine in the matching `.env.txt`.

**Setup:** one partition, 384-d unit vectors drawn around cluster centres (50 points per
centre, per-dimension noise 0.05, so cosine to the centre is about 0.7): closer to sentence
embeddings than uniform random vectors. 500 fresh queries from the same distribution, top-10.
HNSW M=16, ef_construction=200 (the defaults); ground truth = brute force. Single thread,
i7-1165G7 laptop under WSL2 (the same power-limited box as Phases 2–3; p99 values are noisy).

| n | BF p50 | BF p99 | HNSW build | HNSW / BF memory | ef | recall@10 | HNSW p50 | HNSW p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 111 µs | 362 µs | 0.27 s | 1.8 / 1.6 MB | 16 | 0.9988 | 23 µs | 45 µs |
| | | | | | 64 | 1.0000 | 68 µs | 137 µs |
| 5,000 | 410 µs | 1,350 µs | 2.9 s | 13.8 / 12.8 MB | 16 | 0.9934 | 64 µs | 293 µs |
| | | | | | 64 | 1.0000 | 199 µs | 562 µs |
| 10,000 | 1,451 µs | 3,200 µs | 8.2 s | 27.6 / 25.6 MB | 16 | 0.9886 | 81 µs | 389 µs |
| | | | | | 32 | 0.9998 | 111 µs | 284 µs |
| | | | | | 64 | 1.0000 | 224 µs | 690 µs |
| 20,000 | 3,240 µs | 15,178 µs | 24.5 s | 55.1 / 51.2 MB | 32 | 1.0000 | 195 µs | 497 µs |
| | | | | | 64 | 1.0000 | 383 µs | 1,204 µs |
| 50,000 | 9,017 µs | 22,513 µs | 100 s | 112.1 / 102.8 MB | 32 | 0.9978 | 399 µs | 1,197 µs |
| | | | | | 64 | 1.0000 | 889 µs | 2,352 µs |

(All ef values 16–256 are in the JSON.)

**Decisions taken from this**
- `index.hnsw_min_partition_size = 5000` (was a 10,000 placeholder). Brute force's p99 is within
  the 2 ms lookup budget (§3) at 5K (1.35 ms) but over it at 10K (3.2 ms). HNSW at 5K is already
  2× faster at the median with recall 1.0.
- `index.hnsw_ef_search = 64` stays: recall@10 = 1.0000 at every size here, p99 ≤ 2.4 ms even at
  50K. ef=32 is the lever if latency matters more (≥ 0.9978 recall).

**Costs and caveats**
- HNSW memory is ~8% above brute force (links: 2M=32 ids on layer 0).
- Build cost grows super-linearly: 2.9 s for 5K, 100 s for 50K, single-threaded with efC=200.
  The auto upgrade builds a partition's graph once at 5K on the writer thread, outside the
  partition lock (readers keep using brute force); other inserts wait in the bounded writer queue
  for those ~3 s. Tombstone rebuilds at large sizes cost the same; lowering ef_construction or
  incremental compaction are the levers if this shows up in Phase 7.
- Synthetic clusters make recall look ideal. Phase 7 measures recall on real QQP embeddings.
