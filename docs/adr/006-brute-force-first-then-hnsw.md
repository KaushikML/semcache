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
