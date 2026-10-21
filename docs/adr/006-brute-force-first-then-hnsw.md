# ADR-006: Brute-force SIMD first; HNSW behind the same VectorIndex interface

- Status: Accepted
- Date: 2026-10-18

## Context
Sizing (design.md §5.3) estimates that a brute-force scan meets the 2 ms lookup budget up to
roughly 10–13K vectors per partition. Most context partitions will be far smaller, but a
single-system-prompt app concentrates everything in one partition.

## Options
1. Brute force only.
2. HNSW only.
3. A library (FAISS, hnswlib, usearch).
4. Brute force first, then our own HNSW behind the same interface, selected per partition by size.

## Decision
Option 4. Vectors are L2-normalized at insert, so cosine = dot. Brute force stores vectors in
one contiguous 32-byte-aligned `float` buffer (SoA by row), with an AVX2+FMA kernel and a scalar
fallback chosen at runtime (`__builtin_cpu_supports`). Deletes use tombstones plus swap-remove
compaction. HNSW (Phase 5) follows Malkov & Yashunin, with recall@10 validated against brute
force. `index.kind = auto` switches a partition at `index.hnsw_min_partition_size`, set from
measurement.

## Tradeoffs / consequences
- + Brute force is exact, trivially correct and the ground truth for HNSW recall tests.
- + Implementing HNSW ourselves (instead of hnswlib) is the point of the portfolio. Keeping a
  library-free path also avoids licence and build coupling. A library could be added as a third
  implementation for comparison.
- − HNSW deletes are awkward (tombstones degrade the graph). Plan: tombstone, then rebuild the
  partition when the tombstone ratio exceeds a threshold.
- − Two implementations to maintain. The shared interface tests run against both.

## Implementation notes (Phase 5, 2026-10-21)
- `HnswIndex` (`src/index/hnsw_index.*`): layers from `floor(-ln U / ln M)`, greedy descent then
  beam search (`ef_search`), the paper's neighbour-selection heuristic (Algorithm 4, pruned
  connections kept to fill up), bidirectional links pruned to M (2M on layer 0), seeded RNG so
  graphs are reproducible.
- **Deletes are tombstones** that keep routing searches and stay eligible as neighbours, so a
  deleted entry point cannot disconnect the graph; they are filtered from results.
  `wants_rebuild()` fires past `index.hnsw_max_tombstone_ratio` (0.25).
- **Rebuilds and upgrades happen outside the partition lock.** The writer thread (the only
  mutator) builds a fresh index from the partition's live entries, then swaps it in under a brief
  exclusive lock, so readers never wait for a graph build. `index.kind = auto` upgrades a
  partition to HNSW at `hnsw_min_partition_size`. A tombstone rebuild that finds the partition
  below that size goes back to brute force.
- `search()` is const and runs concurrently under the shared lock; its visited set is a
  `thread_local` generation-stamped array (no per-search clearing).
- **Measured** ([bench/results/phase5](../../bench/results/phase5/README.md), synthetic clustered
  384-d data): recall@10 = 1.0000 at ef=64 for 1K–50K vectors; p50 224 µs vs brute force 1,451 µs
  at 10K. Switch point set to **5,000**, where brute force's p99 is still under the 2 ms budget
  and HNSW is already 2× faster at the median. Cost: ~8% more memory and a super-linear build
  (2.9 s at 5K, 100 s at 50K), paid on the writer thread.
