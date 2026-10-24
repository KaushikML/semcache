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
- Phase 3 (cache v1): done. Context key (denylist of transport fields), AVX2/scalar dot,
  `BruteForceIndex`, `CacheStore` (partitions, exact fast path, TTL, CLOCK, quotas), embed pool,
  SSE assembly/re-stream, headers, metrics, `/stats`. Brute force meets 2 ms to ~15K vectors.
- Phase 4 (coalescing + concurrency): done. `CacheWriter` single writer thread; `Coalescer` (flight
  key = context + prompt hash + stream flags; leader records via `LeaderWriter`, followers replay
  live; flight retired after the insert is applied). Coroutines take parameters by value.
- Phase 5 (HNSW): done. `HnswIndex`; `index.kind` auto upgrades a partition at 5000 vectors
  (measured), built off-lock on the writer. Recall@10 = 1.0 at ef=64 on synthetic clusters.
- Phase 6 (persistence + invalidation): done. Snapshot + WAL (`src/persist/`), recovery with torn-tail
  truncation / corrupt-file set-aside / model fingerprint; tags + `POST /admin/purge` + purge guard.
  Known: one 20K HNSW partition recovers in ~17 s (graph rebuilt from vectors).
- Phase 7 (evaluation): done. `bench/results/phase7`. QQP (10K pairs,
  downloaded to data/, not committed) + synthetic FAQ (`bench/data/faq_eval.jsonl`), embedded by the
  product embedder (`semcache_embed_texts`). Threshold default now **0.97** (ADR-012: <= 1% false hits on
  QQP non-dups; recall 12.4%). MiniLM catches near-rewordings only; negation fools it. Load
  (`semcache_loadgen` + `tools/run_load_eval.py`): exact HIT 24.5K rps p50 1 ms; semantic/MISS
  embedding-bound (~300 rps); mixed workload -81% upstream calls; herd 50x20: 20 vs 1000 upstream
  calls. HNSW recall@10 on real QQP vectors >= 0.995 at ef=64. Cost model: `tools/cost_model.py`.
- Phase 8 (dashboard): done. `web/` embedded at build time
  (`cmake/embed_web.cmake`, ADR-010), `/dashboard` with same-origin CSP, `/stats` recent ring (no
  prompts) + `/stats/stream` (SSE, capped), demo mode safety (allow-listed mock upstream, public
  `demo` tenant, bounded per-IP limiter, prompt cap, memory cap, `POST /demo/reset`).
  `docs/demo-script.md`. Browser E2E: `tools/dashboard_e2e.py` (Playwright; Chromium libs unpacked
  without root, see demo-script). Test harness `TestEnv::settle` fixes a COALESCED-vs-HIT race.
- **Phase 9 (packaging): implemented, awaiting review.** Install rules (`$ORIGIN/../lib`, `--strip`),
  two-stage `Dockerfile` (pinned, JOBS=4, non-root, model baked in, bash `/dev/tcp` healthcheck),
  `docker-compose.yml` (demo + mock, read-only, cap_drop), `config/docker*.toml`,
  `scripts/docker-smoke.sh` + CI `docker` job, `docs/deploy.md`, `README.md`, memory eval
  (`bench/results/phase9`: 167 MiB idle, ~8 KB RSS per 2 KB-answer entry). `scripts/docker-smoke.sh`
  passes locally (Docker Desktop with WSL integration; image 343 MB).
- Python tooling: `python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt`.
