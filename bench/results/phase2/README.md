# Phase 2 — embedding latency

Reproduce: `scripts/fetch-model.sh && cmake --preset release && cmake --build build/release && bench/run_embed_bench.sh`.
Raw data: `embed_20261019T182623Z.json` (5 repetitions, medians below); machine details in the matching `.env.txt`.

**Machine:** i7-1165G7 laptop (4C/8T), WSL2, ORT 1.20.1 CPU, `intra_op_threads = 1`, fp32 model.
**Caveat:** this host was running well below the CPU's capability. Single-core fp32 GEMM measured
~35 GFLOP/s (numpy, 32×384 · 384×1536), against roughly 90 GFLOP/s theoretical at base clock, which
suggests power or thermal limits. Python ORT on the same file gives the same latency (18.9 ms at 32
tokens vs 18.5 ms here), so the numbers reflect the host, not the C++ wrapper. Expect a desktop or
server core to be several times faster. Re-run before quoting these anywhere else.

| Benchmark | Median | Throughput |
|-----------|-------:|-----------:|
| Tokenize, 16 tokens | 6.9 µs | 8.5 MB/s |
| Tokenize, 64 tokens | 30.6 µs | 10.1 MB/s |
| Tokenize, 256 tokens | 142.7 µs | 9.1 MB/s |
| Embed 1 text, 16 tokens | 11.1 ms | 90 /s |
| Embed 1 text, 32 tokens | 22.0 ms | 45 /s |
| Embed 1 text, 64 tokens | 37.2 ms | 27 /s |
| Embed 1 text, 128 tokens | 81.8 ms | 12 /s |
| Embed 1 text, 256 tokens | 168.3 ms | 6 /s |
| Batch of 4 × 32 tokens | 72.3 ms | 55 /s |
| Batch of 16 × 32 tokens | 288.4 ms | 55 /s |
| 1 thread, 32 tokens | 18.5 ms | 54 /s |
| 2 threads, 32 tokens | 26.2 ms | 76 /s |
| 4 threads, 32 tokens | 46.1 ms | 87 /s |
| 8 threads, 32 tokens | 75.1 ms | 107 /s |

**Takeaways**
- Latency grows linearly with tokens; tokenization is under 1% of it.
- Batching does not raise throughput on CPU (55/s at batch 1, 4 and 16), so the embed pool
  should embed one prompt per job rather than wait to fill batches.
- Threads scale poorly: 2× throughput from 8 threads. That fits a memory-bandwidth or
  power-limited host. Phase 4 should size `embedder.threads` from a re-measurement.
- On this host the embedder alone exceeds the 10 ms HIT p99 budget (design §3) for any prompt over ~16 tokens.
