# Phase 3 — vector search cost

Reproduce: `cmake --build build/release -j4 && ./build/release/semcache_bench_index --benchmark_repetitions=5`.
Raw data: `index_20261019T195906Z.json` (5 repetitions, medians below); machine in the matching `.env.txt`.

**Machine:** i7-1165G7 laptop, WSL2 (6 vCPUs at measurement time), RelWithDebInfo, single thread, 384-d float32.

| Benchmark | Scalar | AVX2 + FMA | Design §5.3 estimate |
|-----------|-------:|-----------:|---------------------:|
| One dot product (384-d) | 663 ns | 42.6 ns | — |
| Brute-force top-8, 1,000 vectors | 699 µs | 69 µs | ~0.15 ms |
| Brute-force top-8, 10,000 vectors | 6.8 ms | 1.3 ms | ~1.5 ms |
| Brute-force top-8, 20,000 vectors | 15.6 ms | 2.8 ms | ~3 ms |
| Brute-force top-8, 100,000 vectors | 67.8 ms | 16.2 ms | ~30 ms (200K: 30 ms) |

**Takeaways**
- AVX2 is ~15× the scalar kernel: the scalar loop is not auto-vectorized (strict FP ordering).
- The AVX2 scan streams ~10–11 GiB/s once the partition leaves cache (22 GiB/s at 1K, cache-resident),
  matching the §5.3 bandwidth assumption of ~10 GB/s.
- The 2 ms lookup budget (§3) holds up to **~15K vectors per partition** with brute force. Phase 5
  sets `index.hnsw_min_partition_size` from this and the HNSW measurements.
- Scalar fallback misses the budget beyond ~3K vectors; it exists for CPUs without AVX2.
