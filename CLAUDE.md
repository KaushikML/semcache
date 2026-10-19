# semcache — working notes for Claude

Semantic caching proxy for LLM APIs. OpenAI-compatible `POST /v1/chat/completions`;
serves semantically similar prompts (same exact context) from cache, coalesces
in-flight duplicates, fails open to upstream. Portfolio project: correctness,
measurement and clean engineering over feature count.

Source of truth for design: `docs/design.md`. Decisions: `docs/adr/`.

## Stack
- C++20, CMake >= 3.20, vcpkg manifest mode (pinned baseline).
- Boost.Asio + Boost.Beast (server + upstream client), OpenSSL for upstream TLS.
- ONNX Runtime C++ API (official prebuilt release, pinned version + SHA-256, fetched by CMake)
  running all-MiniLM-L6-v2 (384-d). Own WordPiece tokenizer (utf8proc for Unicode).
- nlohmann-json, spdlog (JSON structured logs), toml++ (config).
- GoogleTest, Google Benchmark. ASan/UBSan/TSan in GitHub Actions CI.
- Python ONLY for tooling (model export, dataset prep, golden files, plots). Never in the product.
- Dashboard: static HTML/CSS/vanilla JS served by the binary. No Node/npm/CDN; works offline.
- Linux first (WSL2 dev box: g++ 13, clang, 8 cores, AVX2).
- **Build with at most 4 parallel jobs** (`cmake --build <dir> -j4`, `ninja -j4`): the dev box has
  limited RAM (~8 GB) and higher parallelism has killed sessions. Run sanitizer builds one at a time.

## System design rules
- Requirements, sizing and non-goals live in `docs/design.md`; limits come from the sizing math, not guesses.
- Module boundaries: Embedder, VectorIndex, CacheStore, Upstream, Coalescer, Metrics (+ Http, Config).
  Dependencies point inward; no module reaches into another's internals; composition over inheritance.
- Every significant decision gets a short ADR (context, options, decision, tradeoffs).
- Concurrency model is written down (design.md §7). Respect the lock ordering; never hold a lock across I/O
  or an ORT `Run`. TSan must stay clean.
- FAIL OPEN: any cache-layer failure → request passes through to upstream. The cache never fails a request
  that upstream would have served.
- Bounded everything: queues, body size, in-flight requests, per-key/per-IP rate limits, timeouts.
- Observability: structured logs, `/metrics` (Prometheus), `/healthz`, `/readyz`, `/stats`, `/stats/stream`.
- Security: secrets only from env vars, never logged. Cache partitioned per tenant — cross-tenant reads are
  impossible by construction (tenant id is part of the context key). Prompt text redacted from logs by default.
- All thresholds/TTLs/capacities/ports in config (TOML) with `SEMCACHE_*` env overrides. No magic numbers.
- Single node implemented; multi-node scaling story documented (design.md §11).
- Testing pyramid: unit, integration (mock upstream), load, fault injection.

## Engineering rules
- Small commits, one concern each, clear messages.
- Every phase ships with tests. Never claim something works without running it; show real output.
- NEVER fabricate benchmark numbers. Numbers in README/dashboard come from reproducible scripts or live
  measurement. Estimates in design.md are labelled as estimates.
- Simple and correct over clever; add complexity only when a measurement justifies it.
- Public interfaces first so implementations can be swapped and compared.
- Call out assumptions and known weaknesses explicitly.
- Stop at the end of each phase, summarize, wait for approval.

## Phases
0. Design — design.md + ADRs. **No code.**
1. Walking skeleton — passthrough proxy + mock upstream (latency, deterministic, SSE, fake cost). Tests.
2. Embedding — WordPiece + ONNX embedder; cosine > 0.999 vs sentence-transformers; latency benchmarks.
3. Cache v1 — brute-force SIMD (AVX2 + scalar) search, context partitioning, TTL + LRU, headers, metrics.
4. Coalescing + concurrency — in-flight coalescing, concurrent readers/single writer, TSan, herd tests.
5. HNSW — custom, same VectorIndex interface; recall@10 vs brute force.
6. Persistence + invalidation — snapshot + WAL, tag-based purge API.
7. Evaluation — QQP + synthetic FAQ; hit vs false-hit curve; latency/throughput; BF vs HNSW;
   coalescing on/off; cost model. Results + scripts under `bench/results`.
8. Dashboard — `/dashboard`, demo scenarios, demo-mode safety, tests, `docs/demo-script.md`.
9. Packaging — Docker/compose, `docs/deploy.md`, README.

## Status
- Phase 0 (design): done. Decisions: proxy-issued tenant keys; context key hashes last K=2 prior
  messages (configurable, -1 = all); vcpkg + prebuilt ORT; own WordPiece tokenizer.
- Phase 1 (walking skeleton): done. Deferred `/stats`, `/stats/stream` to Phase 3/8.
- Phase 2 (embedding): done. Own WordPiece (char classes generated from HF), `OnnxEmbedder`
  (ORT 1.20.1, model pinned, ADR-011). Min cosine 0.9999994 vs sentence-transformers. Embed latency
  on the dev laptop is far over the 10 ms HIT budget for long prompts; re-measure on target hardware.
- **Phase 3 (cache v1): implemented, awaiting review.** Context key (denylist of transport fields,
  ADR-002 notes), AVX2/scalar dot, `VectorIndex` + `BruteForceIndex`, `CacheStore` (partitions,
  exact fast path, TTL, CLOCK under global budget + per-tenant quota), bounded `WorkerPool` as
  embed pool, SSE assembly/re-stream, HIT/MISS/BYPASS headers, metrics, `/stats`.
  Interim: mutations synchronous under writer mutex W (design §7.3); Phase 4 adds the writer
  thread and coalescing. Brute force meets the 2 ms lookup budget to ~15K vectors/partition.
  Deferred: tags/purge (Phase 6), `/stats/stream` + recent-requests ring (Phase 8).
- Python tooling: `python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt`.
