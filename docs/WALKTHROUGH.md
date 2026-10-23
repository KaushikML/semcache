# semcache — complete walkthrough (Phases 0–7)

This document explains everything built so far: the idea, the design, every file, every library,
the C++ techniques used, how a request flows through the code, how concurrency and failures are
handled, how it is tested, and what was measured. Read it top to bottom once; afterwards use the
file map (§4) as an index.

Status at time of writing: Phases 0–6 done; Phase 7 (evaluation) implemented and awaiting review.
221 automated tests pass in debug, AddressSanitizer/UBSan and ThreadSanitizer builds.

---

## Table of contents

1. [What semcache is, in plain words](#1-what-semcache-is-in-plain-words)
2. [The core idea: context key + semantic match](#2-the-core-idea-context-key--semantic-match)
3. [The life of one request](#3-the-life-of-one-request)
4. [Repository map: every file and what it does](#4-repository-map-every-file-and-what-it-does)
5. [Libraries and tools, and why each one](#5-libraries-and-tools-and-why-each-one)
6. [C++ techniques you will see everywhere](#6-c-techniques-you-will-see-everywhere)
7. [Build system, scripts, CI](#7-build-system-scripts-ci)
8. [Phase 0 — design documents and ADRs](#8-phase-0--design-documents-and-adrs)
9. [Phase 1 — the walking skeleton (proxy + mock)](#9-phase-1--the-walking-skeleton-proxy--mock)
10. [Phase 2 — the embedder (tokenizer + ONNX model)](#10-phase-2--the-embedder-tokenizer--onnx-model)
11. [Phase 3 — the cache](#11-phase-3--the-cache)
12. [Phase 4 — coalescing and the writer thread](#12-phase-4--coalescing-and-the-writer-thread)
13. [Phase 5 — the HNSW index](#13-phase-5--the-hnsw-index)
14. [Phase 6 — persistence and purge](#14-phase-6--persistence-and-purge)
15. [Phase 7 — evaluation](#15-phase-7--evaluation)
16. [Concurrency model as implemented](#16-concurrency-model-as-implemented)
17. [Failure handling: "fail open"](#17-failure-handling-fail-open)
18. [Configuration reference](#18-configuration-reference)
19. [Observability: logs, metrics, endpoints](#19-observability-logs-metrics-endpoints)
20. [Testing: how we know it works](#20-testing-how-we-know-it-works)
21. [Measurements so far](#21-measurements-so-far)
22. [Known weaknesses and what comes next](#22-known-weaknesses-and-what-comes-next)
23. [How to build, run and test](#23-how-to-build-run-and-test)
24. [Glossary](#24-glossary)

---

## 1. What semcache is, in plain words

Applications that use LLMs (ChatGPT-style APIs) send many requests that are really the *same
question* asked again, sometimes with slightly different wording: "What is your refund policy?",
"what's your refund policy", and so on. Each call costs money and takes 0.3–5 seconds.

**semcache is a proxy that sits between an application and the LLM API.** The application changes
only its `base_url` to point at semcache. semcache speaks exactly the same API as OpenAI's
`POST /v1/chat/completions`. For every request it decides:

- **HIT** — "I have already answered an equivalent question in exactly the same context"; return
  the stored answer in about a millisecond (exact repeat) or about 10 ms (reworded repeat).
- **MISS** — forward to the real API, return its answer to the client, and store it for next time.
- **BYPASS** — this request must not be cached (e.g. the client asked for 2 answers, or the cache
  layer itself had a problem); forward it untouched and store nothing.
- **COALESCED** — several identical requests arrive at once; only the first goes upstream and the
  others receive the same answer, live, as it is produced (Phase 4).

The client sees which one happened in the response header `X-Semcache`.

The hard part is **correctness**: returning a cached answer to a question that only *looks* similar
but means something different, or was asked under different instructions, would be a wrong
answer. Most of the design is about preventing that.

---

## 2. The core idea: context key + semantic match

A cached answer may be reused only if **both** hold:

1. **Exact context match.** Everything that influences the answer, except the final user
   question, must be byte-for-byte equivalent: the tenant (customer), the model, temperature and
   every other parameter, the tools, the system prompt, and the last few earlier messages of the
   conversation. All of that is hashed (SHA-256) into a 32-byte **context key**. Only entries with
   the same context key are even considered. Different tenant → different key → impossible to see
   another tenant's answers.
2. **Semantic match of the final user message.** That message is turned into an **embedding**:
   a list of 384 numbers produced by a small neural network (all-MiniLM-L6-v2) such that
   sentences with similar meaning get vectors pointing in similar directions. Similarity is
   measured by **cosine similarity** (1.0 = identical direction). If the best stored question in
   the same context has cosine ≥ `similarity_threshold` (default 0.97, chosen from measurements in
   Phase 7 — §15), it is a HIT.

There is also a shortcut: if the final message is *literally* the same (after Unicode
normalization and trimming), it is an **exact HIT** and the neural network is not even run.

Example measured with the real model:

| Stored question | New question | Cosine | Result (threshold 0.97) |
|---|---|---:|---|
| How do I reset my password? | How can I reset my password? | 0.9917 | HIT |
| What is your refund policy? | what's your refund policy | 0.9843 | HIT |
| How do I reset my password? | How do I change my email address? | 0.4398 | MISS |
| What is your refund policy? | What is your shipping policy? | 0.5309 | MISS |
| What is your refund policy? | How do I get my money back? | 0.5510 | MISS (a real paraphrase that this small model does not catch) |

---

## 3. The life of one request

```
Client ──HTTP──► ProxyServer (src/proxy/proxy_server.cpp)
                   │  read HTTP request (size limit, timeout)
                   │  route by path: /v1/chat/completions, /healthz, /readyz, /stats, /metrics
                   │  authenticate "Authorization: Bearer <key>" → tenant      (401 if unknown)
                   │  per-tenant rate limit                                     (429 if exceeded)
                   │  in-flight limit                                           (503 if overloaded)
                   ▼
                 RequestPipeline::handle_chat (src/proxy/pipeline.cpp)
                   │  parse JSON; must have non-empty "messages" array          (400 otherwise)
                   │  classify_request → cacheable? (src/cache/context_key.cpp)
                   │     no  → forward upstream as BYPASS, store nothing
                   │     yes → context key + prompt hash + embed text
                   │  CacheStore::exact_lookup(context, prompt hash)
                   │     found → HIT (similarity 1.0)
                   │  Coalescer::join(flight key) (src/coalesce/coalescer.cpp)
                   │     an identical request is already in flight → FOLLOWER:
                   │        replay the leader's response as it arrives → COALESCED
                   │     otherwise → LEADER: everything below is recorded for followers
                   │  on an embed-pool thread (src/common/worker_pool.hpp):
                   │     OnnxEmbedder::embed(text) → 384-d vector
                   │     CacheStore::nearest(context, vector) → best entry + cosine
                   │        (brute force or HNSW, chosen per partition: src/index/)
                   │     cosine ≥ threshold → HIT
                   │     otherwise            → MISS (remember the vector)
                   ▼
   HIT:  serve_hit  — stored answer with a fresh id/created; JSON, or re-streamed as SSE
   MISS: forward    — HttpUpstream sends the request; the response is relayed to the client
                      while being captured; if cacheable it is queued to the writer thread
                      (src/cache/cache_writer.cpp), which inserts it into the CacheStore
                   ▼
                 ProxyServer logs one JSON line, updates metrics

 (on the writer thread, every applied change is also appended to the write-ahead log, and a
  snapshot is written every few minutes: src/persist/ — the cache survives restarts)
```

Every arrow that touches the cache has an escape hatch: if anything in the cache layer fails
(embedder error, queue full, exception), the request is forwarded upstream as **BYPASS**. The
cache can only ever remove work, never break a request.

---

## 4. Repository map: every file and what it does

### Top level

| File | What it is |
|---|---|
| `CLAUDE.md` | Working notes/rules for the AI assistant: stack, design rules, phases, status, build limits (`-j4`). |
| `PROMPT.md` | Your original project brief (git-ignored by your choice). |
| `CMakeLists.txt` | The build: defines the core library, the `semcache` and `semcache-mock` executables, benchmarks and tests. |
| `CMakePresets.json` | Named build configurations: `debug`, `release`, `asan`, `tsan`. |
| `vcpkg.json` | C++ dependency list for vcpkg, pinned to one vcpkg commit (`builtin-baseline`). |
| `.gitignore` | Ignores build dirs, `external/` (vcpkg), `vcpkg_installed/`, `models/`, `.cache/`, `.venv/`, `data/` (but keeps `tests/data/`), `*.onnx`. |
| `.claude/settings.json` | Claude Code setting: no co-author line in commits. |
| `.github/workflows/ci.yml` | GitHub Actions: builds and tests all four presets on Ubuntu 24.04, caching vcpkg, ORT and the model. |

### `cmake/`

| File | What it does |
|---|---|
| `warnings.cmake` | `semcache_set_warnings(target)`: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wold-style-cast …`, and `-Werror` (warnings fail the build). Disables one GCC false positive about coroutine frames. |
| `sanitizers.cmake` | If `SEMCACHE_SANITIZE` is set (`address,undefined` or `thread`), adds `-fsanitize=…` to compile and link flags. |
| `onnxruntime.cmake` | Downloads the official ONNX Runtime 1.20.1 Linux x64 release tarball, verifies its SHA-256, extracts it to `.cache/onnxruntime`, and creates the imported target `onnxruntime::onnxruntime`. |

### `scripts/`

| File | What it does |
|---|---|
| `bootstrap-vcpkg.sh` | Clones vcpkg into `external/vcpkg` at the exact commit in `vcpkg.json` and builds the `vcpkg` tool. |
| `fetch-model.sh` | Downloads `onnx/model.onnx` and `vocab.txt` for all-MiniLM-L6-v2 from HuggingFace at a pinned git revision, and checks both SHA-256 hashes. Skips files already present with the right hash. |

### `config/`

| File | What it does |
|---|---|
| `semcache.example.toml` | Documented example of every config key. |
| `dev.toml` | Local development config: demo mode, port 8080, upstream = mock on 9090, one tenant `dev` whose key is in `SEMCACHE_KEY_DEV`. |

### `docs/`

| File | What it does |
|---|---|
| `design.md` | The source of truth: problem, requirements, non-goals, sizing math, architecture, data flows, concurrency, failure modes, observability, security, multi-node story, config, tests. Measured numbers are recorded next to the estimates. |
| `adr/000-template.md` … `adr/011-…` | Architecture Decision Records (see §8). |
| `WALKTHROUGH.md` | This file. |

### `src/` — the product (C++)

| Directory / file | Module | What it does |
|---|---|---|
| `main.cpp` | composition root | Parses `--config`, loads config, creates every object, wires them, runs the I/O threads, handles SIGINT/SIGTERM shutdown. |
| `net/asio.hpp` | Net | Single include point for Boost.Asio/Beast; namespace aliases (`net`, `beast`, `http`, `ssl`, `tcp`); `Task<T>` = coroutine type; `use_nothrow` completion token. |
| `net/listener.{hpp,cpp}` | Net | Accepts TCP connections and runs one coroutine per connection on its own strand; tracks live sessions so shutdown can cancel them. Shared by proxy and mock. |
| `http/response_writer.hpp` | Http | Abstract interface for writing a response (`send`, `begin_stream`, `write_chunk`, `end_stream`). Lets the pipeline be tested without sockets. |
| `http/beast_response_writer.{hpp,cpp}` | Http | The real implementation over a Beast TCP stream; streaming uses HTTP chunked encoding; every write has a timeout. |
| `http/errors.{hpp,cpp}` | Http | `openai_error_json(...)`: builds `{"error":{"message","type","code"}}` like OpenAI does. |
| `config/config.{hpp,cpp}` | Config | All settings as structs with defaults; TOML loading; `SEMCACHE_<SECTION>__<KEY>` env overrides; rejects unknown keys; validation. |
| `common/secret.hpp` | — | `Secret`: holds an API key; has no printing support, so it cannot land in a log by accident; `reveal()` must be called explicitly. |
| `common/sha256.{hpp,cpp}` | — | SHA-256 via OpenSSL's EVP API; `to_hex`. |
| `common/worker_pool.{hpp,cpp}` | — | Bounded thread pool. Coroutines `co_await pool.run(fn)`; `fn` runs on a pool thread; the coroutine resumes on its own executor; a full queue rejects immediately. Used as the embed pool. |
| `log/log.{hpp,cpp}` | Log | One JSON object per log line via spdlog (`ts`, `level`, `msg`, fields). |
| `metrics/metrics.{hpp,cpp}` | Metrics | Lock-free `Counter`, `Gauge`, `Histogram` (atomics) and a registry that renders the Prometheus text format; histogram quantiles for `/stats`. |
| `auth/tenants.{hpp,cpp}` | Http/auth | Maps `Bearer <key>` → tenant by looking up SHA-256(key); raw keys are never stored. |
| `limits/rate_limiter.{hpp,cpp}` | Http/limits | Token-bucket rate limiter keyed by tenant, sharded over 16 mutexes. |
| `upstream/upstream.hpp` | Upstream | Interface: `send(request, sink)`; the response is delivered piece by piece to an `UpstreamSink`. Error kinds: connect, timeout, protocol, reset, aborted. |
| `upstream/http_upstream.{hpp,cpp}` | Upstream | Real HTTP/1.1 client (plain or TLS) with connect / first-byte / idle / total timeouts and a header allow-list. |
| `embed/embedder.hpp` | Embedder | Interface `embed(texts) → vectors`; `Embedding` = `std::array<float, 384>`; `EmbedError` (`too_long`, `inference`). |
| `embed/bert_normalizer.{hpp,cpp}` | Embedder | Text normalization identical to HuggingFace's BERT normalizer, plus pre-tokenizer character classes. |
| `embed/bert_tables.inc` | Embedder | GENERATED code-point range tables used by the normalizer (from `tools/gen_bert_tables.py`). |
| `embed/wordpiece_tokenizer.{hpp,cpp}` | Embedder | Text → token ids (`[CLS] … [SEP]`) using the 30,522-word vocab. |
| `embed/onnx_embedder.{hpp,cpp}` | Embedder | Runs the model with ONNX Runtime, mean-pools and L2-normalizes; refuses texts over 256 tokens. |
| `cache/context_key.{hpp,cpp}` | ContextKeyBuilder | Decides cacheable vs bypass; builds the context key, prompt hash and embed text; canonical JSON. |
| `cache/cache_store.{hpp,cpp}` | CacheStore | Entries partitioned by context key; exact lookup; nearest-neighbour lookup; insert; TTL; CLOCK eviction under byte budgets; metrics. |
| `cache/cache_writer.{hpp,cpp}` | CacheStore | The single writer thread: all inserts and TTL sweeps go through its bounded queue, applied in order. |
| `coalesce/coalescer.{hpp,cpp}` | Coalescer | Flight map, per-flight response log, `LeaderWriter` (records what the leader's client receives), `follow()` (replays it to followers). |
| `cache/completion_codec.{hpp,cpp}` | CacheStore helpers | Reassembles streamed responses into one JSON; decides if a response may be stored; renders HITs as JSON or SSE. |
| `index/vector_index.hpp` | VectorIndex | Interface: `add`, `remove`, `search(q, k)`. |
| `index/simd.{hpp,cpp}` | VectorIndex | Dot-product kernels: scalar and AVX2+FMA; runtime CPU detection. |
| `index/brute_force_index.{hpp,cpp}` | VectorIndex | Exact search over one contiguous float buffer. |
| `persist/crc32c.{hpp,cpp}` | Persistence | CRC-32C checksum on every record on disk. |
| `persist/codec.{hpp,cpp}` | Persistence | Bounds-checked binary encoding of cache entries (`ByteWriter` / `ByteReader`). |
| `persist/log_format.{hpp,cpp}` | Persistence | WAL segment and snapshot file formats; readers that stop at the first bad record. |
| `persist/files.{hpp,cpp}` | Persistence | Durable file writes: write-all, `fdatasync`, directory `fsync`. |
| `persist/persistence.{hpp,cpp}` | Persistence | Recovery, WAL buffering and fsync, segment rotation, snapshot thread, disk-error fallback. |
| `index/hnsw_index.{hpp,cpp}` | VectorIndex | Approximate search over a layered proximity graph (HNSW); tombstone deletes. |
| `proxy/proxy_metrics.hpp` | Metrics | Every metric the proxy exports, registered once. `CacheResult` enum (HIT/MISS/COALESCED/BYPASS). |
| `proxy/proxy_server.{hpp,cpp}` | Http | Connection loop, routing, auth, rate limit, in-flight limit, `/healthz`, `/readyz`, `/stats`, `/metrics`, request log line. |
| `proxy/pipeline.{hpp,cpp}` | RequestPipeline | The only place that composes all modules for one chat request: classify → lookup → HIT / MISS / BYPASS. |

### `mock/` — the fake LLM API

| File | What it does |
|---|---|
| `mock_upstream.{hpp,cpp}` | An OpenAI-compatible server that returns deterministic fake answers (same model + same last user message → same answer), with configurable latency, per-token delay, SSE streaming, a fake cost header, and fault injection. |
| `main.cpp` | The `semcache-mock` executable and its command-line flags. |

### `tools/` — Python tooling (never part of the product)

| File | What it does |
|---|---|
| `requirements.txt` | Pinned Python packages (torch CPU, transformers, sentence-transformers, onnx, onnxruntime, datasets, numpy). |
| `make_golden.py` | Generates `tests/data/tokenizer_golden.jsonl` (3,125 strings with HuggingFace token ids) and `tests/data/embedding_golden.jsonl` (125 sentences with reference embeddings); also checks that the pinned ONNX file reproduces sentence-transformers. |
| `prepare_eval.py` | Phase 7: downloads a seeded QQP sample (pinned revision) into `data/eval/` (not committed) and writes the FAQ set. |
| `faq_eval_data.py` | Phase 7: the synthetic FAQ evaluation set (intents, paraphrases, hard negatives). |
| `analyze_threshold.py` | Phase 7: hit vs false-hit curves (QQP pairs, FAQ cache replay) and plots. |
| `run_load_eval.py` | Phase 7: starts the real binaries per scenario and drives the C++ load generator. |
| `cost_model.py` | Phase 7: savings vs compute, from measured rates + labelled assumptions. |
| `gen_bert_tables.py` | Runs every Unicode code point through HuggingFace's normalizer and pre-tokenizer; writes `src/embed/bert_tables.inc` and `tests/data/normalizer_blocks.txt`; `--diff` mode for debugging mismatches. |

### `bench/` — benchmarks

| File | What it does |
|---|---|
| `embed_bench.cpp` | Google Benchmark: tokenizer speed, single embeds at 16–256 tokens, batching, 1–8 concurrent threads. |
| `index_bench.cpp` | Google Benchmark: dot product scalar vs AVX2; brute-force search at 1K–100K vectors. |
| `embed_texts.cpp` | Embeds a JSONL file with the product's `OnnxEmbedder` (multi-threaded) → float32 file. |
| `loadgen.cpp` | Closed-loop HTTP load generator: modes cycle / case / once / herd / mix; latency per `X-Semcache` result. |
| `data/faq_eval.jsonl` | The committed FAQ evaluation set. |
| `persist_eval.cpp` | WAL replay, snapshot write and snapshot load times and sizes for N entries. |
| `hnsw_eval.cpp` | Brute force vs HNSW per partition size: recall@10, p50/p99 latency for several `ef`, build time, memory. |
| `run_embed_bench.sh` | Runs the embed benchmark, saves JSON plus machine details. |
| `results/phase2/` … `phase7/` | Raw JSON results, machine info, and a README with the tables and takeaways. |

### `tests/`

| Path | What it does |
|---|---|
| `CMakeLists.txt` | Builds `semcache_unit_tests` and `semcache_integration_tests`, registers each test with CTest, sets sanitizer options. |
| `support/test_env.hpp` | In-process harness: starts a mock upstream + proxy on random ports, a small async HTTP client, helpers (`chat`, `sse_events`, …). |
| `support/fake_embedder.hpp` | Deterministic fake embedder (aliases at exact cosine, forced errors, a gate to block). |
| `support/recording_writer.hpp` | A `ResponseWriter` that just records calls, for coalescer tests without sockets. |
| `support/model_paths.hpp` | Paths to the real model and golden files. |
| `sanitizers/lsan.supp`, `tsan.supp` | Narrow suppressions for noise from inside the prebuilt ONNX Runtime library. |
| `unit/*.cpp`, `integration/*.cpp` | The tests (§20). |
| `data/` | Golden files generated by the Python tools. |

---

## 5. Libraries and tools, and why each one

| Library / tool | Used for | Why this one |
|---|---|---|
| **C++20** (g++ 13, clang) | The product | Coroutines, `std::span`, concepts, designated initializers. Performance and control over memory/threads. |
| **CMake ≥ 3.20 + Ninja** | Build | Industry standard; presets give named, reproducible configurations. |
| **vcpkg** (manifest mode) | C++ package manager | Declarative `vcpkg.json`, pinned baseline commit → everyone gets identical versions (ADR-003). |
| **Boost.Asio** | Async networking, timers, thread pool of I/O threads, coroutines | Mature, the basis of C++ networking; `co_await` support. |
| **Boost.Beast** | HTTP/1.1 parsing and writing on top of Asio | Low-level, fast, supports streaming bodies and chunked encoding. |
| **Boost.URL** | Parsing `upstream.base_url` | Correct URL parsing. |
| **OpenSSL** | TLS to the real API; SHA-256 | Standard; also gives a fast hash. |
| **nlohmann/json** | JSON parsing/building | Easy API; objects are `std::map` (sorted keys), which helps canonical JSON. |
| **spdlog** | Logging backend | Fast; we write one JSON object per line on top of it. |
| **toml++** | Config file parsing | Header-only, good error messages with line numbers. |
| **utf8proc** | Unicode: UTF-8 decoding, NFD decomposition, lowercase, NFC | Small C library; we use it only for mappings HuggingFace also applies. |
| **ONNX Runtime 1.20.1** (prebuilt) | Running the neural network | Official fast CPU inference; prebuilt avoids a 30-minute source build (ADR-003). |
| **all-MiniLM-L6-v2** | Embedding model (384-d) | Small (22M params, ~90 MB), fast, widely used for sentence similarity. |
| **GoogleTest** | Unit/integration tests | Standard C++ test framework; parameterized tests. |
| **Google Benchmark** | Microbenchmarks | Repetitions, statistics, JSON output. |
| **ASan/UBSan/TSan** | Finding memory errors, undefined behaviour, data races | Compiler-built-in sanitizers; CI runs all of them. |
| **Python** (torch, transformers, sentence-transformers) | Only for generating golden reference data | The reference implementation of the model lives there; never shipped. |
| **GitHub Actions** | CI | Builds and tests on every push. |

---

## 6. C++ techniques you will see everywhere

**Coroutines (`Task<T>` = `boost::asio::awaitable<T>`).** Network code is asynchronous (one thread
can handle thousands of connections) but written top-to-bottom like normal code:
`auto [ec, n] = co_await http::async_read(...)`. When the operation would block, the coroutine
suspends and the thread does other work; when data arrives, it resumes. `co_return` returns a value.

**`use_nothrow`.** Asio operations normally throw on error. Our token
`as_tuple(use_awaitable)` makes them return `(error_code, result)` instead, so errors are values
we check explicitly.

**Strands.** An Asio *strand* guarantees that handlers posted to it never run concurrently. Each
connection runs on its own strand, so a session's state needs no locks even though the
`io_context` runs on many threads.

**Cancellation signals.** Each session gets a `cancellation_signal`. On shutdown, `Listener::stop`
emits "terminal" cancellation to every session, which aborts whatever it is waiting on (including
an upstream call).

**RAII.** Resources are released by destructors. Example: `InflightSlot` in `proxy_server.cpp`
increments the in-flight counter in its constructor and decrements it in its destructor, so every
exit path (including errors) gives the slot back.

**Interfaces + composition.** `Upstream`, `Embedder`, `VectorIndex`, `ResponseWriter` are abstract
classes. The pipeline receives them by pointer/reference from `main.cpp`. Tests pass fakes
(`FakeEmbedder`, a mock upstream). No module includes another's internals.

**`shared_ptr<const CacheEntry>`.** Readers get a shared pointer to an immutable entry and can use
it after releasing all locks; if the cache evicts the entry meanwhile, the memory lives until the
last reader drops its pointer.

**`std::shared_mutex`.** Many readers can hold the lock at once (`shared_lock`), a writer gets it
alone (`unique_lock`). Lookups are reads; inserts and evictions are writes.

**Atomics.** Metrics and the cache's "recently used" bit are `std::atomic`, updated without locks
(`memory_order_relaxed`, since only the counts matter, not ordering with other memory).

**SIMD intrinsics.** `dot_avx2` uses `_mm256_fmadd_ps` to multiply-add 8 floats per instruction,
with 4 independent accumulators. It is compiled with `__attribute__((target("avx2,fma")))` so the
rest of the program still runs on CPUs without AVX2; `__builtin_cpu_supports` chooses at runtime.

**`thread_local`.** The brute-force index needs a scratch array of scores per search. Because
searches run concurrently under a *shared* lock, a member variable would be a data race; a
`thread_local` buffer gives each thread its own.

**`std::span`, `std::string_view`.** Non-owning views of arrays/strings: pass data without copying.

**Error results instead of exceptions across modules.** `EmbedResult`, `UpstreamResult`,
`PoolResult` carry an error enum. Exceptions are only used inside modules and are caught at the
boundary (fail open).

**Coroutine parameters by value.** A coroutine keeps running after the expression that started it
has finished, so a reference parameter can point at a temporary that no longer exists. The sanitizers
caught exactly this in Phase 4 (`follow()` took a `const std::function&`); coroutines that can
outlive their caller's arguments now take parameters by value or as `shared_ptr`.

**GCC 13 and `co_await`.** GCC 13 crashes (an internal compiler error) on some temporaries inside a
`co_await` expression — braced initializers, coroutine lambdas. The code therefore builds named
locals first and then awaits (`const auto msg = …; co_await w.send(…, msg, …)`).

**`concurrent_channel` as a wakeup.** A coalescing follower waits on an Asio
`experimental::concurrent_channel` with capacity 1: the leader `try_send`s after each new chunk. A
wakeup sent while the follower is busy is kept in the channel, so none is ever lost; duplicates are
dropped.

**`[[nodiscard]]`, `-Werror`, strict warnings.** Ignoring a returned result or an implicit narrowing
conversion is a compile error.

---

## 7. Build system, scripts, CI

**Libraries/targets (`CMakeLists.txt`):**
- `semcache_core` (static library): everything in `src/` except `main.cpp`.
- `semcache` (executable): `src/main.cpp` + core.
- `semcache_mock` (static library) + `semcache-mock` (executable).
- `semcache_bench_embed`, `semcache_bench_index`, `semcache_hnsw_eval` (benchmarks, option `SEMCACHE_BUILD_BENCH`).
- `semcache_unit_tests`, `semcache_integration_tests` (option `SEMCACHE_BUILD_TESTS`).

**Presets (`CMakePresets.json`):** each preset builds into `build/<name>`:
- `debug` (g++, debug info, no optimization),
- `release` (RelWithDebInfo: optimized + symbols; used for benchmarks),
- `asan` (clang, AddressSanitizer + UndefinedBehaviorSanitizer),
- `tsan` (clang, ThreadSanitizer, optimized).

All use the vcpkg toolchain, so `find_package(...)` finds the vcpkg-installed libraries.

**ONNX Runtime fetching (`cmake/onnxruntime.cmake`):** at configure time, if
`.cache/onnxruntime/onnxruntime-linux-x64-1.20.1/` is missing, download the tarball from GitHub,
check `EXPECTED_HASH SHA256=67db4dc1…`, extract, and declare the shared library as an imported
target. Its headers are marked SYSTEM so our strict warnings do not apply to them.

**Sanitizer suppressions (`tests/sanitizers/`):** ONNX Runtime is a prebuilt, uninstrumented
library. It makes ~128 bytes of one-time global allocations it never frees (LeakSanitizer would
complain), and its internal memory arena frees memory on a different thread than allocated it,
synchronized in ways TSan cannot see. We suppress only reports whose stack is inside
`libonnxruntime.so`; anything with our own frames still fails. `tests/CMakeLists.txt` attaches the
suppression files to each test via the `ENVIRONMENT` property.

**CI (`.github/workflows/ci.yml`):** a matrix over `debug, release, asan, tsan`: install ninja +
clang, restore caches (vcpkg binary cache; model + ORT tarball), `scripts/fetch-model.sh`,
`scripts/bootstrap-vcpkg.sh`, configure, build, `ctest`. (Not yet run on GitHub.)

**Dev-box rule:** build with `-j4` at most; the WSL box has limited RAM and parallel builds have
killed sessions.

---

## 8. Phase 0 — design documents and ADRs

`docs/design.md` was written before any code. Key sections:

- **Requirements (F1–F14):** OpenAI-compatible endpoint incl. streaming; proxy-issued tenant keys;
  context key; exact fast path; embedding + nearest neighbour; threshold; store on miss;
  coalescing; headers; bypass header and tags; eviction; persistence and purge; ops endpoints;
  demo mode.
- **Non-functional targets:** HIT adds ≤ 10 ms p99; index lookup ≤ 2 ms; MISS adds ≤ 12 ms;
  ≥ 500 req/s all-HIT (estimate); 1 GiB default memory budget; **fail open**.
- **Non-goals:** multi-node implementation, other APIs, semantic matching of earlier turns,
  caching multimodal / `n>1` / `logprobs`.
- **Sizing:** ~5 KB per entry → ~200K entries per GiB; brute force scan ~1.5 ms per 10K vectors
  (estimate) → brute force first, HNSW later; request limits derived from these numbers.
- **Architecture:** module table, data flows for HIT/MISS/COALESCED, request classification,
  what gets stored, re-streaming.
- **Concurrency:** threads, shared state, lock order, backpressure table.
- **Failure modes:** a table with detection, behaviour, what the client sees, and the metric.
- **Observability, security, scaling past one node, config schema, test strategy.**

**ADRs (Architecture Decision Records):** one short file per significant decision, each with
context → options → decision → trade-offs.

| ADR | Decision |
|---|---|
| 001 | Clients use proxy-issued keys mapped to tenants; the real upstream key stays on the server. |
| 002 | Context key = SHA-256 over tenant, all parameters, system messages and last K=2 prior messages (`-1` = all). Known risk documented: conversations that differ only more than K messages ago can share answers. Phase 3 notes: denylist of transport fields, whole messages hashed, canonical numbers. |
| 003 | vcpkg for libraries; prebuilt ONNX Runtime pinned by version + SHA-256. |
| 004 | Write our own WordPiece tokenizer, verified by golden files. Phase 2 notes: why Unicode categories failed and how tables are generated from HuggingFace. |
| 005 | Threads: async I/O pool + bounded embed pool + single writer + maintenance; coroutines. |
| 006 | Brute-force SIMD index first; HNSW later behind the same interface. |
| 007 | Eviction: TTL + CLOCK (approximate LRU) under a byte budget and per-tenant quotas. |
| 008 | Persistence: snapshot + write-ahead log (Phase 6). |
| 009 | Coalescing on exact flight key; near-duplicate coalescing optional (Phase 4). |
| 010 | Dashboard as static files served by the binary (Phase 8). |
| 011 | Use the ONNX export published with the model, pinned by revision + SHA-256 (no torch export step). |
| 012 | Default similarity threshold 0.97: the lowest with ≤ 1% false hits on QQP's look-alike non-duplicates. |

---

## 9. Phase 1 — the walking skeleton (proxy + mock)

Goal: a proxy that forwards everything correctly, with every limit and timeout in place, plus a
deterministic fake upstream so tests, benchmarks and demos are free and reproducible.

### 9.1 Configuration (`src/config/`)

`Config` is a struct of sections: `ServerConfig`, `UpstreamConfig`, `EmbedderConfig`,
`CacheConfig`, `LimitsConfig`, `LoggingConfig`, `std::vector<TenantConfig>`. Every field has a
default in the struct definition — the single place for defaults.

`load_config_from_string` parses TOML with toml++, and the `Reader` class reads each key with
this precedence: **env var `SEMCACHE_<SECTION>__<KEY>` > file > default**. It records every key it
knows; `reject_unknown_keys()` then fails on any key in the file it did not read (so a typo like
`max_inflght` is an error, not silently ignored). Types are checked (string/bool/int range/double/
millisecond and second durations/string arrays; arrays from env are comma-separated).

Secrets: the upstream key comes only from `SEMCACHE_UPSTREAM_API_KEY`. Tenant keys are read from
the env var named by `key_env`, and only their SHA-256 digest is kept.

`finalize_config` validates (positive limits, threshold in (0,1], etc.) and derives
`upstream.host/port/tls/path_prefix` from `base_url` using Boost.URL.

### 9.2 Small utilities

- `Secret` — a string wrapper with no `operator<<`; you must call `reveal()`.
- `sha256()` — OpenSSL `EVP_Digest`.
- `log::event(level, msg, fields)` — builds an `nlohmann::ordered_json` line
  `{"ts","level","msg",...fields}`; invalid UTF-8 is replaced instead of crashing.
  Prompt text is never put in fields (unless `logging.log_prompts`).

### 9.3 Metrics (`src/metrics/`)

- `Counter` (monotonic), `Gauge` (up/down), `Histogram` (fixed buckets, counts per bucket, sum).
  All atomics; the hot path never locks.
- `MetricsRegistry::counter/gauge/histogram(name, help, labels)` registers once at startup and
  returns a reference. Same name + different labels = one family (e.g.
  `semcache_requests_total{result="HIT"}`).
- `render_prometheus()` writes the text format scraped by Prometheus (`# HELP`, `# TYPE`,
  cumulative `_bucket{le=…}`, `_sum`, `_count`).
- `Snapshot::quantile(q)` estimates percentiles by interpolating inside a bucket (used by `/stats`).
- Latency buckets: 0.1 ms … 60 s, covering both sub-ms lookups and slow upstream calls.

### 9.4 Auth and rate limiting

- `TenantDirectory::authenticate("Bearer sk-…")` → hashes the key and looks it up in a map keyed by
  digest. No raw key is ever compared or stored.
- `RateLimiter` — token bucket per tenant: `burst` tokens max, refills at `rps` per second; a
  request takes one token; if none, the answer includes how long until one is available
  (`Retry-After`). 16 shards (each with its own mutex) reduce contention.

### 9.5 Networking

- `Listener` binds a TCP port (port 0 = random, used by tests), accepts in a loop on a strand, and
  for each connection spawns `handler(stream, remote)` on a new strand with a cancellation slot.
  It keeps a map of live sessions; `stop()` closes the acceptor and cancels every session.
- `BeastResponseWriter` implements `ResponseWriter`: `send()` writes a complete response;
  `begin_stream()` writes headers with `Transfer-Encoding: chunked` and `Cache-Control: no-cache`;
  `write_chunk()` writes one HTTP chunk (never an empty one, which would end the stream);
  `end_stream()` writes the final zero chunk. Each write has `write_timeout`. Once a write fails,
  `healthy()` is false and the connection is closed.

### 9.6 The upstream client (`src/upstream/http_upstream.cpp`)

One connection per request (simple and correct; a keep-alive pool only if measurements show it
matters):

1. DNS resolve → TCP connect, bounded by `connect_timeout`.
2. For `https`: TLS with SNI (server name indication) and certificate host-name verification.
3. Write the request: our own `Authorization: Bearer <upstream key>` (the client's proxy key is
   never forwarded), `Accept: application/json, text/event-stream`, the client's body unchanged.
4. Read headers within `first_byte_timeout`.
5. Read the body in 16 KB pieces, each within `idle_timeout`; every timer is also capped by what
   is left of `total_timeout` (the `arm()` lambda).
6. Each piece goes to `sink.on_body(...)`; if the sink returns false (client gone), abort.
7. Only allow-listed headers are relayed (`x-request-id`, `openai-processing-ms`, `openai-model`);
   cookies and upstream account details are dropped.
8. Errors are classified: `connect`, `timeout`, `protocol`, `reset`, `aborted`.

### 9.7 The proxy server (`src/proxy/proxy_server.cpp`)

`session()` loops per connection (HTTP keep-alive): read a request with `max_body_bytes`
(413 if larger) and `read_timeout`; `route()` it; stop if the response says close.

`route()`:
- `/v1/chat/completions` — POST only (405 otherwise) → `handle_chat`.
- `GET /healthz` → `{"status":"ok"}` (process alive).
- `GET /readyz` → `ready`, or `degraded` with `["embedder"]` if the model did not load — still
  HTTP 200, because the node still serves every request (fail open).
- `GET /stats` → JSON summary (Phase 3).
- `GET /metrics` → Prometheus text.
- anything else → 404 OpenAI-style error.

`handle_chat()`: authenticate (401), rate limit (429 + `Retry-After`), take an in-flight slot (503
+ `Retry-After: 1` beyond `max_inflight`), read the `X-Semcache-Bypass` header, call the pipeline,
count the result, observe request latency, and log one line: request id, tenant, status, cache
result, streamed?, latency, upstream time, upstream error, bytes in, similarity, bypass reason,
store outcome. Never the prompt.

### 9.8 `main.cpp` — composition root and shutdown

Order: parse args → load config (exit 2 on error) → init logging → warn if no upstream key →
create `io_context` with N threads → TLS context → `HttpUpstream` → `MetricsRegistry` → load
`OnnxEmbedder` (on failure: warn, continue exact-only) → `CacheStore` → embed `WorkerPool` (only
with an embedder) → `ProxyServer` → start → start the TTL sweeper (on its own strand) → install
SIGINT/SIGTERM handler → run the io_context on N threads.

Shutdown on a signal: stop accepting and cancel sessions, stop the sweeper, stop the embed pool
(queued embeds complete as "rejected" → those requests fail open), then wait for sessions to drain
up to `shutdown_timeout`, then exit. `std::_Exit(0)` avoids destroying objects that abandoned
coroutine frames might still reference.

### 9.9 The mock upstream (`mock/`)

- **Deterministic:** the answer's seed is FNV-1a(model + "\n" + last user message); a splitmix64
  random generator picks words from a 32-word vocabulary ("Mock answer: … refund policy …").
  `created` is fixed. Same request → byte-identical answer.
- **Token counts:** whitespace word counts (a stand-in for a real tokenizer) → `usage`.
- **Latency:** `--latency-ms` before the first byte, `--token-delay-ms` between streamed tokens.
- **Streaming:** OpenAI-shaped SSE: a role chunk, one chunk per token, a final chunk with
  `finish_reason: "stop"`, an optional usage chunk (if `stream_options.include_usage`), `[DONE]`.
- **Fake cost:** `X-Mock-Cost-Usd` from `--usd-per-1k-input/output`.
- **Faults** (only with `--allow-faults`), via a body field `"mock_fault": "status=503;reset-after=3;delay=500;hang"`:
  return an error status, drop the connection after N chunks, add delay, or never answer. A body
  field is used because the proxy forwards bodies verbatim but not client headers.
- `/mock/stats` (request count), `/mock/reset`, `/healthz`.

---

## 10. Phase 2 — the embedder (tokenizer + ONNX model)

The neural network does not read text; it reads **token ids**. The model was trained with a
specific tokenizer (BERT uncased WordPiece). If our C++ tokenizer differs from HuggingFace's even
slightly, embeddings drift silently. So Phase 2 is mostly about **exact parity**.

### 10.1 The model and how we get it (ADR-011)

`scripts/fetch-model.sh` downloads two files from HuggingFace at the pinned revision
`1110a243…` and checks SHA-256:
- `models/all-MiniLM-L6-v2.onnx` (fp32, 90 MB): inputs `input_ids`, `attention_mask`,
  `token_type_ids`; output `last_hidden_state` of shape [batch, tokens, 384].
- `models/vocab.txt`: 30,522 tokens, one per line; line number = token id.

Originally the plan was to export the model ourselves with torch; the model repo already publishes
the ONNX file, so we pin it instead — no torch needed in CI.

### 10.2 Text normalization (`src/embed/bert_normalizer.cpp`)

HuggingFace's BERT normalizer does, in order:
1. **clean_text** — drop control/format characters, map whitespace to `' '`.
2. **handle_chinese_chars** — put spaces around CJK ideographs (each becomes its own word).
3. **strip_accents** — Unicode NFD decomposition (`é` → `e` + combining accent), drop the marks.
4. **lowercase**.

**The problem we hit:** HuggingFace's Rust code uses *older* Unicode tables than any current
library. It keeps unassigned characters, keeps most "spacing marks", strips U+1734, and does not
know Unicode 16 scripts. Our first version used Unicode general categories via utf8proc and
produced different token ids for **803 of 3,125** test strings.

**The fix:** `tools/gen_bert_tables.py` asks HuggingFace itself, for *every one of the 1,114,112
Unicode code points*, what its normalizer and pre-tokenizer do with it, and writes compact range
tables (`src/embed/bert_tables.inc`, ~1,000 ranges):
- `kRemoved` — dropped (controls, stripped marks, U+FFFD),
- `kWhitespace` — becomes a space,
- `kCjkPadded` — gets spaces around it,
- `kMapped` — changed by NFD/lowercase,
- `kPunctuation` — split into its own word by the pre-tokenizer,
- `kSplitSpace` — word separators.

The C++ normalizer looks characters up in these tables (binary search over ranges) and calls
utf8proc for NFD + lowercase **only for characters in `kMapped`**. That way newer utf8proc
knowledge (e.g. Unicode 16 case mappings) never changes our output. A final step re-applies NFD
canonical ordering for any retained marks.

Ordering detail discovered by testing: HuggingFace pads CJK characters *before* NFD, so CJK
compatibility ideographs (U+F900…) are both padded and decomposed.

### 10.3 WordPiece (`src/embed/wordpiece_tokenizer.cpp`)

1. Normalize the text (above).
2. **Pre-tokenize:** split on separators; every punctuation character is its own word.
3. **WordPiece per word:** greedy longest match from the left against the vocab; pieces after the
   first get a `##` prefix (`unaffable` → `un`, `##aff`, `##able`). If any part cannot be matched,
   the whole word becomes `[UNK]`. Words over 100 characters become `[UNK]`.
4. Wrap with `[CLS]` … `[SEP]`.

Invalid UTF-8 bytes are dropped. No truncation inside the tokenizer — the embedder decides.

### 10.4 The ONNX embedder (`src/embed/onnx_embedder.cpp`)

- **Load:** read vocab; check the model file exists; create an ORT session with
  `intra_op_threads = 1` (parallelism comes from our pool, not from ORT), sequential execution,
  full graph optimization, no busy-spinning. Check the model has the expected input/output names.
  Run a probe embed of `""` to verify the output dimension is 384. Any failure throws
  `EmbedderLoadError` → `main` continues without an embedder.
- **embed(texts):** tokenize each text; if any exceeds `max_tokens` (256, the model's training
  length) return `too_long` — **never truncate**, because two long prompts with the same first 256
  tokens would wrongly match. Pad the batch to the longest sequence with `[PAD]`, attention mask
  0 for padding. Run ORT. **Mean pooling:** average the 384-d vectors of the real tokens.
  **L2 normalization:** divide by the vector length, so cosine similarity = dot product.
- **Thread-safety:** ORT's `Session::Run` is thread-safe; the tokenizer is read-only; buffers are
  per call. So many pool threads can call `embed` at once.

### 10.5 Golden files and the parity gates

`tools/make_golden.py` (Python, using the real HuggingFace libraries) writes:
- `tests/data/tokenizer_golden.jsonl`: 70 hand-written edge cases (accents, Turkish İ, Greek
  sigma, emoji, zero-width chars, controls, CJK, 101-char words…), 55 natural prompts, and 3,000
  seeded random strings mixing vocab words with characters from 50 Unicode blocks. Not QQP data,
  to avoid committing Quora text.
- `tests/data/embedding_golden.jsonl`: 125 sentences with sentence-transformers embeddings.
- It also verifies that the pinned ONNX file + HF token ids + mean pooling reproduces
  sentence-transformers (min cosine 0.9999999), so any C++ mismatch can only be ours.

Gates (all pass):
- token ids identical on all 3,125 strings;
- the normalizer identical on **every code point**: `tests/data/normalizer_blocks.txt` holds the
  SHA-256 of HuggingFace's output per block of 4,096 code points; the C++ test recomputes all 272
  blocks. On a mismatch it dumps the block and `gen_bert_tables.py --diff` lists the differences;
- C++ embeddings vs sentence-transformers: **min cosine 0.9999994** (gate: > 0.999).

### 10.6 Readiness

If the model is missing, `/readyz` returns 200 with `"degraded":["embedder"]` and the gauge
`semcache_embedder_ready` is 0. The proxy still caches exact repeats.

---

## 11. Phase 3 — the cache

### 11.1 Classification and the context key (`src/cache/context_key.cpp`)

`classify_request(body, tenant, cfg, bypass_header)` returns either a `CacheableRequest`
(context key, prompt hash, embed text) or a reason it is not cacheable:

| Reason | When |
|---|---|
| `cache_disabled` | `cache.enabled = false` |
| `bypass_header` | client sent `X-Semcache-Bypass: 1` |
| `multiple_choices` | `n` present and not 1 |
| `logprobs` | `logprobs: true` |
| `temperature` | above `cache.max_temperature` |
| `last_not_user` | final message is not `role: user` (e.g. a tool result) |
| `non_text_content` | final message contains images/audio parts |

**Context key encoding** (then SHA-256): each field is written as an 8-byte length followed by
its bytes, so `("ab","c")` and `("a","bc")` differ:
1. version tag `semcache-context-v1`,
2. tenant id,
3. **all top-level request fields except** `messages, stream, stream_options, user, metadata,
   store`, as canonical JSON — so a new API parameter automatically splits the cache instead of
   colliding (a deliberate change from the design's original allow-list),
4. every system/developer message (whole message object),
5. the last K non-system messages before the final one (K = `history_k`, default 2).

**Canonical JSON:** keys sorted, no whitespace, integers as integers, floats in shortest form
(`1` and `1.0` encode the same). A missing parameter is simply absent — different from an explicit
default value.

**Embed text / prompt hash:** the final user message text, Unicode NFC-normalized and trimmed;
the prompt hash is its SHA-256 (used for the exact fast path).

### 11.2 SIMD dot product (`src/index/simd.cpp`)

`dot_scalar` is a plain loop. `dot_avx2` processes 32 floats per iteration with four 8-wide FMA
accumulators, then adds the lanes together and handles a scalar tail. `detected_simd()` checks
the CPU once. Measured: **42.6 ns** (AVX2) vs **663 ns** (scalar) per 384-d dot product.

### 11.3 Vector index (`src/index/`)

`VectorIndex` interface: `add(id, vector)`, `remove(id)`, `search(query, k)` → best k
(id, score), `size`, `dim`, `memory_bytes`. Not thread-safe by itself; the cache guards it.

`BruteForceIndex`: all vectors in one contiguous `std::vector<float>` (row-major), ids in a
parallel vector, `id → row` map. Remove = copy the last row into the hole (stays dense). Search =
score every row with the dot kernel, keep the top k with a small sorted insertion array. Exact,
so it is also the ground truth for the future HNSW index.

### 11.4 The cache store (`src/cache/cache_store.cpp`)

**Data layout:**
- `partitions_`: hash map `ContextKey → Partition` (one partition per context).
- `Partition`: its own `shared_mutex`; `entries` (id → entry), `exact` (prompt hash → id), and a
  `VectorIndex` (created lazily, only if an entry has an embedding).
- `CacheEntry` (immutable after insert, except two atomics): id, context, prompt hash, tenant,
  prompt text, optional embedding, the stored response JSON, total tokens, created/expires times,
  accounted bytes, `referenced` bit, hit count.

**Reads** (`exact_lookup`, `nearest`): lock the partition map shared → copy the partition pointer
→ unlock → lock the partition shared → find → copy the entry pointer → unlock → check expiry.
`nearest` fetches 8 candidates so a few expired-but-not-yet-swept entries cannot hide a live one.

**Insert:**
1. Compute accounted bytes = 350 (overhead estimate from the design) + tenant + prompt + response
   + 1,536 if there is an embedding. If larger than the budget → `too_large`, not stored.
2. Take the writer mutex **W**.
3. If the same (context, prompt hash) exists, remove it (newest answer wins).
4. If the tenant has an explicit quota, evict that tenant's entries until it fits.
5. Evict globally until it fits in `cache.max_bytes`.
6. Create the partition if needed, add to `entries`, `exact`, and the index.
7. Put the entry into the CLOCK ring just behind the hand (so it is visited last).

**CLOCK eviction (approximate LRU, ADR-007):** entries sit in a circular list with a "hand". A
hit only sets the entry's atomic `referenced` bit — no lock. To evict: look at the entry under the
hand; if expired → evict it; if referenced → clear the bit and move on (second chance); otherwise
evict it. Two full turns always find a victim. The tenant-quota variant skips other tenants'
entries.

**TTL:** each entry expires at `created + ttl` (per-tenant override or `default_ttl_s`). Reads
ignore expired entries immediately (lazy); `expire(now)` removes them (called by the sweeper every
`sweep_interval_ms`, and the eviction loop removes expired entries first).

**Metrics:** `semcache_index_entries`, `semcache_index_bytes`, `semcache_partitions` gauges;
`semcache_evictions_total{reason="ttl|capacity|tenant_quota"}`;
`semcache_store_skipped_total{reason="entry_over_budget"}`.

A bug found by tests: eviction caused by the global budget was first labelled `tenant_quota`
(because a tenant's default quota equals the global budget); now the per-tenant loop runs only for
tenants with an explicit quota.

### 11.5 The embed pool (`src/common/worker_pool.hpp`)

Running the neural network takes milliseconds of CPU; doing that on an I/O thread would stall
every other connection on that thread. So:

```cpp
auto r = co_await pool.run<EmbedSearch>([&] { /* embed + nearest, on a pool thread */ });
// back on the request's own executor here
```

How it works: `run` uses Asio's `async_initiate` to capture the coroutine's completion handler,
wraps the user function into a job, and pushes it into a bounded queue (mutex + condition
variable). A pool thread runs it and **posts** the result back to the coroutine's executor (with a
"work guard" so the io_context does not exit meanwhile). Guarantees:
- **Full queue → `rejected` immediately** (no unbounded waiting) → the request fails open.
- Exceptions inside the job → `failed`.
- `stop()` → queued jobs complete as `rejected`; every waiting coroutine is resumed exactly once.
- Threads are named `embed-0`, `embed-1`, … (visible in `top`, `gdb`).

### 11.6 Response capture and re-streaming (`src/cache/completion_codec.cpp`)

**`StreamAssembler`:** when a MISS is streamed (SSE), the bytes are forwarded to the client
*and* fed into the assembler, which splits events on blank lines (any chunk boundaries, CRLF too),
parses each `data:` JSON, and accumulates: id/created/model/system_fingerprint, role, content,
refusal, tool calls (merged by `index`: id, type, name, and argument fragments concatenated),
`finish_reason`, `usage`. At `[DONE]` it produces the same JSON shape a non-streamed response has.
An error event, missing `[DONE]`, bad JSON, or more bytes than allowed → not storable.

**`make_storable`:** exactly one choice; `finish_reason` in `cache.store_finish_reasons` (default
`stop`, `tool_calls` — a `length`-truncated answer is never cached); JSON size ≤
`cache.max_response_bytes` (64 KB). Extracts `usage.total_tokens`.

**Serving a HIT:**
- `hit_body` — the stored JSON with a fresh `id` (`chatcmpl-sc-…`) and current `created`; the
  original `usage` is kept (it is what was saved).
- `hit_stream_events` — the stored answer as SSE chunks: role chunk; content split near spaces into
  ~16-byte pieces (`restream_chunk_chars`), never inside a UTF-8 character; refusal; tool calls; the
  final chunk with `finish_reason`; a usage chunk if the client asked for it; `[DONE]`.

Property test: re-stream → reassemble gives back the identical completion for ASCII, CJK, emoji
and tool calls at chunk sizes 1 to 1,000.

### 11.7 The pipeline (`src/proxy/pipeline.cpp`)

`handle_chat(tenant, body, bypass_header, out, received)`:
1. Parse JSON; 400 if not an object with a non-empty `messages` array.
2. No store configured → forward as BYPASS.
3. `classify_request` → not cacheable → count `semcache_bypass_total{reason}`, forward as BYPASS.
4. `lookup()`:
   - exact lookup → HIT with similarity 1.0;
   - no embedder → MISS (exact-only mode);
   - otherwise on the pool: embed, then `nearest`. Pool rejected → BYPASS `embed_queue_full`;
     exception → BYPASS `cache_error`; `too_long` → MISS without vector (exact-only entry);
     inference error → BYPASS `embed_error`; best ≥ threshold → HIT; else MISS (keep the vector and
     the best score).
5. HIT → `serve_hit`: render first (if rendering fails, fall back to forwarding), mark the entry
   referenced, add `usage.total_tokens` to `semcache_tokens_saved_total`, send JSON or SSE (with an
   optional delay between chunks, then a final comment `: semcache-total-ms=N` because headers went
   out before the body).
6. MISS → `forward` with capture, then store if: client still connected, status 200, stream
   finished cleanly, storable. Each skip is counted in `semcache_store_skipped_total{reason}`
   (`status`, `incomplete`, `too_large`, `finish_reason`, `shape`, `upstream_error`).

Headers on every chat response: `X-Semcache` (HIT/MISS/BYPASS), `X-Semcache-Latency-Ms` (time to
response headers), `X-Semcache-Similarity` (HIT: matched score; MISS: best score found, if a search
ran).

(Phase 4 inserted coalescing between the exact lookup and the semantic lookup, and moved inserts
to the writer thread — see §12.7.)

### 11.8 `/stats` and the sweeper

`/stats` returns: requests by result, total, hit rate, tokens saved, in-flight, embedder ready,
cache entries/bytes/max/partitions/threshold, and p50/p90/p99 for request, upstream, embed and
lookup latency (estimated from histogram buckets). It never contains prompt or answer text.

The sweeper coroutine in `main.cpp` wakes every `sweep_interval_ms` and (since Phase 4) queues a
TTL sweep to the writer thread, the only thread that mutates the store.

---

## 12. Phase 4 — coalescing and the writer thread

### 12.1 Why coalescing

A cache only helps *after* the first answer is stored. A burst of identical requests (a page load
fanning out, retries, many users asking the same thing at once) arrives before that, so every one
of them would go upstream and pay full price. Coalescing makes them share one upstream call.

### 12.2 The flight key

A **flight** is one upstream call shared by several requests. Requests join the same flight if they
have the same **flight key** = context key + prompt hash + `stream` + `stream_options.include_usage`.
The two stream flags are included because followers receive the leader's *bytes* verbatim; a
streamed and a non-streamed request need different response formats, so they get separate flights
(they still share cache entries afterwards). Only cacheable requests coalesce; BYPASS never does.
Coalescing is decided right after the exact lookup, *before* any embedding work.

### 12.3 Leader and followers

`Coalescer::join(key)` (one mutex-guarded hash map, lock **L1**):
- **First request → leader.** It serves normally (semantic lookup, HIT or MISS…), but through a
  `LeaderWriter` that wraps its real response writer. Every write — status and headers, the body,
  each SSE chunk, the end — is also appended to the flight's log (`Flight`, lock **L1b**).
  The leader's own `X-Semcache-*` headers are stripped from the log; everything else is shared.
- **Later identical requests → followers.** `follow()` replays the log into the follower's own
  connection: it writes what is already there, then waits for a wakeup, writes the new chunks, and
  so on until the leader finishes. Followers therefore stream at the leader's pace. They get their
  own headers: `X-Semcache: COALESCED` and their own latency.

What followers see is *exactly* what the leader's client saw: a HIT, a MISS answer, a 502/504, or
a stream that was interrupted — no retries, so a herd never becomes a retry storm.

### 12.4 Failure cases

| Situation | What happens |
|---|---|
| Leader cancelled / crashes before writing any response head | Followers serve the request themselves (fail open), counted in `semcache_coalesce_independent_total`. |
| Leader dies mid-stream | Followers get the chunks so far, then an SSE error event, then the connection closes. |
| Leader's own client disconnects | The upstream read continues while followers still need it (or while the response is being captured for the cache), bounded by the upstream total timeout. |
| Too many followers (`coalescer.max_waiters_per_flight`) | Extra requests are served independently. |
| Streamed log exceeds `upstream.max_response_bytes` | Followers get an error event; the leader carries on alone. |

### 12.5 Flight lifetime and the "insert window"

Inserts are asynchronous (next section), so there is a moment after the leader finished when the
answer is not yet visible in the cache. A duplicate arriving then would miss and call upstream
again. To close that window, a flight stays joinable until the writer thread has **applied** the
leader's insert: the insert carries a callback that retires the flight. Late duplicates simply
replay the finished flight.

### 12.6 The writer thread (`CacheWriter`)

Design §7 always called for a single writer. Phase 3 used a mutex as a stand-in; Phase 4 makes it
real:
- one thread named `cache-writer`, a bounded queue (`cache.writer_queue_capacity`, 4096);
- jobs: `Insert{entry, after_callback}` and `Expire` (deduplicated while queued);
- full queue → the insert is dropped and counted (`semcache_store_dropped_total`): the request was
  already served, only a future HIT is lost;
- the after-callback runs for every attempted insert, even if it failed — otherwise a flight could
  stay open forever;
- `flush()` waits until everything submitted so far is applied (used by tests and shutdown);
  `stop()` applies what is queued, then exits.

Request threads never take exclusive index locks any more; the mutation order is total (Phase 6
will append it to a write-ahead log in that same order).

### 12.7 The pipeline order now

1. parse + classify (BYPASS if not cacheable)
2. **exact lookup** → HIT (no coalescing needed: answering is instant)
3. **join flight** → follower: replay → COALESCED; leader: continue through `LeaderWriter`
4. **semantic lookup** on the embed pool → HIT (shared with followers) or MISS
5. **forward** upstream, relay + capture, **queue the insert** with "retire flight" as its callback

### 12.8 Tests and bugs found

Herd tests (`tests/integration/herd_test.cpp`) start real clients against the proxy: 50 identical
requests → **1 upstream call**, 1 MISS + 49 COALESCED, one identical body; streaming followers
receive tokens live (headers long before the end); a leader 503 reaches all 20 clients with one
upstream call; leader stream reset → every follower gets the error event; follower cap; tenants and
BYPASS never coalesce; a semantic HIT is shared; the leader hanging up does not cancel the shared
call; and a mixed-traffic stress test (duplicates, streams, constant eviction, TTL sweeps) for TSan.

Mutation checks: making every request a leader fails 8 of 12 herd tests. Letting the leader stop
when its client leaves was *not* caught by the integration test at first (the cache capture also
kept the upstream call alive), so a test where the leader has nothing to store was added; it now
catches it.

Bugs caught on the way: a coroutine reference parameter that dangled (ASan + TSan, see §6); an
oversized upstream response that hung instead of returning 502 (my regression, caught by the
existing failure test); and, in CI, a TSan report caused by spdlog initializing lazily from two
threads in a test (fixed by initializing logging before any thread starts, like `main` does).

---

## 13. Phase 5 — the HNSW index

### 13.1 Why an approximate index

Brute force compares the query with every vector in the partition: cost grows linearly. Most
partitions are small, but one busy app with one system prompt puts everything in one partition.
Measured in Phase 3, brute force passes the 2 ms lookup budget at around 10–15K vectors. HNSW
(Hierarchical Navigable Small World, Malkov & Yashunin) finds the nearest neighbours by walking a
graph, touching only a small fraction of the vectors — at the price of being *approximate*: it can
occasionally miss the true best match. We measure how often (**recall**).

### 13.2 How HNSW works (plain version)

- Every vector is a node. Each node is linked to a few (M=16) nearby nodes, chosen so links point in
  different directions — a "small world" where any node is a few hops from any other.
- Nodes get a random **level**; level 0 contains everyone, level 1 about 1/16 of the nodes, level 2
  about 1/256… Higher levels are sparse "highways".
- **Search:** start at the top level's entry point, greedily hop to whichever neighbour is closer to
  the query until no neighbour is closer; drop a level and repeat. On level 0, run a **beam search**
  that keeps the best `ef_search` (64) candidates and expands them until nothing better is found.
  Return the best k.
- **Insert:** search for the new vector's neighbours the same way (with a wider beam,
  `ef_construction` = 200), link it to M of them using the **neighbour-selection heuristic**
  (skip a candidate if it is closer to an already-chosen neighbour than to the new node — this keeps
  links diverse), and add the reverse links, pruning any list that grows beyond M (2M on level 0).

### 13.3 Our implementation (`src/index/hnsw_index.cpp`)

- Vectors in one contiguous array; links as per-node, per-level lists; external id ↔ node maps.
- Scores use the same AVX2/scalar dot kernel as brute force.
- **Deletes are tombstones:** the node stays in the graph (it still routes searches and may still be
  linked to — removing it could disconnect the graph, e.g. if it is the entry point) but is never
  returned. `wants_rebuild()` turns true when more than 25% of nodes are tombstones.
- **Concurrent searches:** `search()` is const; the "visited" marks it needs live in a
  `thread_local` array with a generation counter, so searches neither share state nor clear an O(n)
  array every time.
- **Reproducible:** the level generator is seeded (`index.hnsw_seed`), so the same inserts build the
  same graph.

A bug the shared index tests caught: re-adding an id tombstoned the only node (the entry point), and
the new node, refusing to link to tombstones, ended up unreachable. Tombstones now stay eligible as
neighbours, as in hnswlib.

### 13.4 Brute force or HNSW, per partition

`index.kind`: `brute`, `hnsw`, or **`auto`** (default): a partition starts with brute force (exact,
cheap to update) and upgrades to HNSW when it reaches `index.hnsw_min_partition_size` vectors.

Rebuilds (the upgrade, or too many tombstones) are done by the **writer thread** *outside* the
partition lock: it is the only thread that changes partitions, so it can read the entries safely,
build a fresh index, and then swap it in under a brief exclusive lock. Readers keep searching the
old index meanwhile and never wait for a graph build. A tombstone rebuild of a partition that has
shrunk below the threshold goes back to brute force.

### 13.5 How it was evaluated (`bench/hnsw_eval.cpp`)

For each size, 384-d unit vectors are drawn around cluster centres (≈ topics, closer to real
sentence embeddings than uniform random vectors), brute force gives the true top-10 for 500
queries, and HNSW is measured at several `ef` values. Results (dev laptop, single thread):

| n | brute force p50 / p99 | HNSW ef=64 recall@10 | HNSW p50 / p99 | build |
|---:|---:|---:|---:|---:|
| 1,000 | 111 / 362 µs | 1.0000 | 68 / 137 µs | 0.27 s |
| 5,000 | 410 / 1,350 µs | 1.0000 | 199 / 562 µs | 2.9 s |
| 10,000 | 1,451 / 3,200 µs | 1.0000 | 224 / 690 µs | 8.2 s |
| 20,000 | 3,240 / 15,178 µs | 1.0000 | 383 / 1,204 µs | 24.5 s |
| 50,000 | 9,017 / 22,513 µs | 1.0000 | 889 / 2,352 µs | 100 s |

Decision: `hnsw_min_partition_size = 5000` — the largest measured size where brute force's p99 is
still inside the 2 ms budget, and HNSW is already 2× faster at the median. Costs: ~8% more memory,
and a build time that grows faster than linearly (the one-off upgrade at 5K costs ~3 s of writer
time, during which other inserts queue). The unit test gate is recall@10 ≥ 0.95 on 5K clustered
64-d vectors (measured 0.9575).

### 13.6 The knobs

| Parameter | Default | Effect |
|---|---|---|
| `hnsw_m` | 16 | Links per node: higher = better recall, more memory, slower inserts. |
| `hnsw_ef_construction` | 200 | Beam while inserting: graph quality vs build time. |
| `hnsw_ef_search` | 64 | Beam while searching: recall vs latency (32 gave ≥ 0.9978 recall, ~2× faster). |
| `hnsw_max_tombstone_ratio` | 0.25 | Rebuild threshold. |
| `hnsw_min_partition_size` | 5000 | Auto switch point. |

---

## 14. Phase 6 — persistence and purge

### 14.1 What it adds

1. **Persistence:** the cache survives a restart (a cold cache costs money): a **write-ahead log
   (WAL)** of every change plus periodic **snapshots**.
2. **Invalidation:** entries can carry **tags**, and an admin endpoint **purges** entries by
   tenant, tag or context — e.g. after a policy changes, purge everything tagged `refund-policy`.

### 14.2 How changes reach the disk

- `CacheStore` notifies a `MutationObserver` inside every change it applies (on the writer
  thread). `Persistence` is that observer:
  - an insert becomes an **INSERT** record (the full entry, including its 384-d vector, bit-exact);
  - an eviction, TTL expiry or purge becomes a **DELETE** record keyed by (context, prompt hash);
  - replacing an entry is just the new INSERT (replay overwrites by key).
- Each record gets a **log sequence number (LSN)**, increasing by one per record.
- Records are appended to a memory buffer while the store is locked; between mutations the
  writer `write`s and `fdatasync`s the buffer, at most every `persistence.fsync_interval_ms`
  (1 s). That is the worst-case loss on a crash.

On disk, `data/` contains:
- `wal-<first LSN in hex>.log` — segments; header (magic, version, first LSN, model fingerprint,
  CRC) then records `[len][crc32c][type][lsn][payload]`;
- `snapshot.bin` — header (magic, version, LSN, count, fingerprint, CRC) then CRC-framed entries.

### 14.3 Snapshots without stopping the world

Every `snapshot_interval_s` (or when a segment exceeds `max_wal_bytes`) the writer:
1. syncs the WAL, then copies the list of entry pointers — entries are immutable
   `shared_ptr<const CacheEntry>`, so this is a cheap, **exactly point-in-time** copy at LSN L;
2. starts a new segment at L+1;
3. hands the copy to the **snapshot thread**, which writes `snapshot.bin.tmp`, fsyncs, renames it
   over `snapshot.bin` (atomic), fsyncs the directory, and deletes segments that are fully
   covered by the snapshot (first LSN ≤ L).

The writer only pauses for the pointer copy; the serialization needs no lock at all.

### 14.4 Recovery at startup

1. Load `snapshot.bin` (all or nothing). Bad magic/version/CRC or a record-count mismatch → the
   file is renamed `snapshot.bin.corrupt-<time>` and startup continues without it.
2. Replay WAL segments in LSN order, skipping records ≤ the snapshot's LSN.
   - A **torn tail** in the last segment (a crash mid-write) → truncate at the first bad record,
     keep everything before it.
   - **Damage in an earlier segment** → stop replay there and set the rest aside. Continuing
     would skip the DELETEs in the damaged part, which could bring **purged** entries back; a
     smaller cache is the safe failure.
3. Entries whose TTL passed meanwhile are skipped; vector indexes are rebuilt by re-inserting.
4. **Model fingerprint:** every file records the SHA-256 of the model file. If the model changed,
   stored vectors would be meaningless, so they are dropped and those entries are served by exact
   match only.

### 14.5 When the disk fails

Any write/fsync/rename error (disk full, read-only filesystem) → persistence is **disabled**:
the cache keeps working from memory (`semcache_persistence_enabled 0`), the persistence files are
removed best-effort (so a crash cannot resurrect entries purged while disabled), and every
`retry_interval_ms` it tries again; on success it writes a full snapshot.

### 14.6 Tags and purge

- `X-Semcache-Tags: refund-policy,faq` on a request attaches those tags to the stored answer.
  Tags are validated (characters `A-Za-z0-9._:/-`, ≤ 64 bytes, ≤ 16 tags). Invalid tags make the
  request **BYPASS**: storing an entry the client could not purge later would be worse.
- `POST /admin/purge` with `Authorization: Bearer $SEMCACHE_ADMIN_KEY` and a JSON filter, any of
  `{"tenant": "...", "tag": "...", "context": "<64 hex>"}` (all given fields must match; an empty
  filter is rejected). Without `SEMCACHE_ADMIN_KEY` the endpoint does not exist (404).
- The purge runs on the writer thread; the reply `{"purged": N}` comes after it was applied, so
  the next lookup cannot see purged entries. Removals are logged as DELETEs, so purges survive
  restarts.
- **Purge guard:** a request that was waiting on upstream when the purge ran would otherwise
  store its (now outdated) answer afterwards. For `cache.purge_guard_window_s`, inserts from
  requests that **started before** a purge and match its filter are dropped.

### 14.7 Measured

| 20K entries, 2 KB responses | 200 brute-force partitions | 1 HNSW partition |
|---|---:|---:|
| inserts through the writer (WAL on) | 8,237 /s | 1,022 /s |
| on disk (WAL / snapshot) | 73.6 / 73.4 MB | same |
| recover from WAL / from snapshot | 0.26 / 0.25 s | 17.1 / 17.7 s |
| write a snapshot | 1.6 s | 1.2 s |

Disk is cheap; **HNSW insertion** (~0.85 ms per vector here) dominates both the recovery of a big
partition and live insert throughput into it. Not fixed yet — levers: recover in the background
while serving passthrough, persist the graph links, or lower `hnsw_ef_construction`.

### 14.8 Tests

`tests/unit/persistence_test.cpp`: CRC-32C check value; codec round trip, every truncation
rejected, random bytes never crash; restart restores entries and replays deletes (purged entries
stay purged); snapshot + newer WAL; **torn tail cut at every 7th byte** of the last record;
bit flip; corrupt snapshot set aside; mid-log damage stops replay; other model → vectors dropped;
expired entries skipped; read-only directory → memory-only → retry → snapshot; purge guard;
purge filter combinations.
`tests/integration/restart_purge_test.cpp`: real proxy — HIT after a restart with zero upstream
calls; purge by tag and by tenant; purge survives restart; in-flight response not stored after a
purge; admin auth and validation; endpoint disabled without a key; invalid tags → BYPASS.
Mutation checks: disabling the purge guard, or not logging removals, each fail a unit test and an
integration test.

---

## 15. Phase 7 — evaluation

The question Phase 7 answers: **does it work, how well, and at what cost?** All results are in
`bench/results/phase7/README.md`; everything is reproducible from the commands listed there.

### 15.1 Data

- **QQP** (Quora Question Pairs, GLUE): 10,000 seeded pairs of real user questions labelled
  "duplicate" (same meaning) or not; its non-duplicates are often about the same topic, which
  makes them good look-alike tests. Downloaded at a pinned revision into the git-ignored `data/`
  (Quora's terms), never committed.
- **FAQ set** (`bench/data/faq_eval.jsonl`, committed): 30 support intents, each a canonical
  question + 4 paraphrases (from "How can I…" to completely different words), and 30 **hard
  negatives** that reuse an intent's words but mean something else ("How do I *pause* my
  subscription?" vs "…*cancel*…").
- All texts are embedded by **the product's C++ embedder** (`semcache_embed_texts`), so the curves
  describe semcache itself.

### 15.2 Choosing the threshold (the most important result)

Two views:
- **QQP pairs:** at threshold t, *recall* = share of duplicate pairs scoring ≥ t (hits the cache
  should make); *false-hit rate* = share of non-duplicate pairs scoring ≥ t (wrong answers it
  would serve).
- **FAQ cache replay:** the cache's real behaviour — seed with the canonical questions, then each
  paraphrase/negative either HITs its nearest stored question (a false hit if that is another
  intent) or MISSes and is stored.

| threshold | QQP recall | QQP false-hit | FAQ hit rate | FAQ false-hit |
|---:|---:|---:|---:|---:|
| 0.80 | 0.763 | 0.211 | 0.180 | 0.047 |
| 0.90 (old placeholder) | 0.425 | 0.063 | 0.040 | 0.007 |
| **0.97 (new default)** | **0.124** | **0.007** | 0.027 | 0.007 |

Rule (ADR-012): the lowest threshold where ≤ 1% of look-alike non-duplicates would get a cached
answer → **0.97**. A wrong answer is worse than a missed saving, so this is a false-hit budget,
not an accuracy optimum.

What it reveals about all-MiniLM-L6-v2 (the honest part):
- it catches **near-rewordings** ("How can I cancel…" vs "How do I cancel…" = 0.996);
- it misses real paraphrases ("How do I talk to a human?" vs "How do I contact customer
  support?" = 0.215; median paraphrase similarity 0.655);
- it is fooled by **negation** ("What does the warranty *not* cover?" vs "…cover?" = 0.974).

So with this model, savings come mostly from **exact repeats** and **coalescing**; the semantic
layer is a safe bonus for rewordings. A stronger model or a re-ranker would be the next lever.

### 15.3 Load tests

`semcache_loadgen` (C++, closed loop) against the real `semcache` + `semcache-mock` binaries,
orchestrated by `tools/run_load_eval.py`. Trick for the semantic path: random letter case gives a
new prompt hash (no exact hit) but the same embedding (the tokenizer is uncased), so every request
runs embed → search → semantic HIT.

| scenario | throughput | p50 | p99 | upstream calls |
|---|---:|---:|---:|---:|
| exact HIT | 24,518 req/s | 1.0 ms | 8.6 ms | 0 |
| semantic HIT | 307 req/s | 103 ms | 150 ms | 0 |
| MISS (upstream 0 ms) | 262 req/s | 120 ms | 195 ms | all |
| mixed (300 ms upstream), cache on | 490 req/s | 48 ms | 488 ms | 2,868 / 14,936 |
| mixed, cache off | 210 req/s | 303 ms | 327 ms | 6,348 / 6,348 |
| herd 50×20, coalescing on / off | — | 509 / 574 ms | 514 / 786 ms | **20 / 1,000** |

Exact HITs are ~1 ms through the whole HTTP stack; anything that needs an embedding is
CPU-bound on the embedder (~13 ms per embed here, plus queueing).

### 15.4 Brute force vs HNSW, and HNSW on real data

At ~9.8K entries in one partition, the lookup p50 is 1.41 ms (brute force) vs 0.83 ms (HNSW), but
end-to-end throughput is identical (~210 req/s) because embedding dominates. HNSW recall@10 on
**real QQP embeddings** at ef=64 is 0.9956–0.9994 (1K–18K vectors), a little below the synthetic
1.0 of Phase 5, so the default stays.

### 15.5 Cost model

`tools/cost_model.py` combines the measured rates with **assumed** traffic, token counts and
prices (labelled as assumptions; plug in your own) into a table of upstream spend saved, proxy
CPU spent and wrong answers per month for several traffic mixes (`cost_model.md`). Embedding CPU is
~$1 per 10M requests at the example prices, so the traffic mix decides the savings.

### 15.6 Smaller findings

- A semantic HIT does not store an exact alias, so repeated paraphrases pay an embedding every
  time; adding one is a cheap improvement (not done).
- Load generator, proxy and mock share the laptop's cores; absolute numbers vary ~2× with its
  power state, so compare within a table.

---

## 16. Concurrency model as implemented

**Threads:**

| Threads | Count | Run | Never |
|---|---|---|---|
| I/O threads | `server.io_threads` (0 = all cores) | accept, parse, auth, rate limit, upstream I/O, writing responses, exact lookups, coalescing (leaders record, followers replay) | model inference, cache mutations |
| Embed pool | `embedder.threads` (4) | tokenize + ORT Run + vector search (brute force or HNSW) | network I/O |
| Writer | 1 (`cache-writer`) | every cache mutation: inserts, evictions, TTL sweeps, purges, index upgrades/rebuilds; WAL fsync between mutations | network I/O |
| Snapshot | 1 | serializes a point-in-time copy of entry pointers to `snapshot.bin` | touch the store |
| Main thread | 1 | setup, then also runs the io_context | — |

**Shared state and protection:**

| State | Protection |
|---|---|
| Config | immutable after startup |
| Metrics | atomics (relaxed) |
| Rate-limit buckets | 16 sharded mutexes |
| In-flight counter | atomic |
| Partition map | `shared_mutex` **L2** |
| One partition (entries, exact map, index) | `shared_mutex` **L3** |
| Entry `referenced` bit / hit count | atomics |
| CLOCK ring, byte totals, mutations | writer mutex **W** (uncontended: only the writer thread mutates) |
| Coalescer flight map | mutex **L1** |
| One flight (log, state, follower list) | mutex **L1b** |
| Embed queue, writer queue | mutex + condition variable |
| Listener session map | mutex |

**Lock order: W → L2 → L3**, and separately **L1 → L1b** (never held together with W/L2/L3).
Readers never take W. At most one L3 at a time. No lock is held across network I/O, an ORT `Run`,
an HNSW build (built off-lock, swapped under a brief L3), or while waking followers.

All of this runs clean under ThreadSanitizer, including stores hammered by readers and the writer,
readers during HNSW upgrades, concurrent HNSW searches, and the herd stress test.

---

## 17. Failure handling: "fail open"

Rule: **if upstream could have served the request, the client gets upstream's answer.**

| Failure | What happens | Client sees |
|---|---|---|
| Model missing at startup | Run without embedder; exact repeats still cached | normal responses; `/readyz` "degraded" |
| Embedding error | Forward, store nothing | `BYPASS` |
| Embed queue full | Forward, store nothing | `BYPASS` |
| Exception in cache code | Forward, store nothing | `BYPASS` |
| Stored entry fails to render | Forward instead | `BYPASS` |
| Upstream 4xx/5xx | Passed through, never stored | upstream's status/body |
| Upstream timeout before headers | — | 504 OpenAI-style error |
| Upstream unreachable / reset before headers | — | 502 |
| Upstream dies mid-stream | SSE error event, connection closed, nothing stored | truncated stream + error event |
| Upstream response too large to buffer | — | 502 |
| Client disconnects | Upstream call continues if followers need it or it is being cached; otherwise aborted | — |
| Coalescing leader fails upstream | Same error for every follower, one upstream call | leader's 5xx/502/504 |
| Coalescing leader vanishes before responding | Followers serve themselves | normal response |
| Writer queue full | Insert dropped (request already served) | — (`semcache_store_dropped_total`) |
| Disk full / write error | Persistence off, memory-only, retried | — (`semcache_persistence_enabled 0`) |
| Corrupt snapshot at startup | Set aside, start without it | — |
| Torn WAL tail at startup | Truncated at first bad record | — |
| Model changed between runs | Stored vectors dropped, exact match only | — |
| Invalid `X-Semcache-Tags` | Not stored | `BYPASS` |
| Too many in-flight requests | The one place we fail closed | 503 + `Retry-After` |
| Over rate limit | — | 429 + `Retry-After` |
| Body too large | — | 413 |
| Bad JSON | — | 400 |
| Unknown key | — | 401 |

---

## 18. Configuration reference

Sections (see `config/semcache.example.toml` for every key with comments):

- top level: `mode` (`live` | `demo`)
- `[server]`: host, port, io_threads, max_inflight, max_body_bytes, read/write/shutdown timeouts
- `[upstream]`: base_url, verify_tls, connect/first_byte/idle/total timeouts, max_response_bytes
- `[embedder]`: model_path, vocab_path, max_tokens (256), intra_op_threads (1), threads (4), queue_capacity (1024)
- `[cache]`: enabled, similarity_threshold (0.97), history_k (2), max_temperature (2.0),
  default_ttl_s (86400), max_bytes (1 GiB), max_response_bytes (64 KiB), store_finish_reasons,
  restream_chunk_chars (16), restream_delay_ms (0), sweep_interval_ms (60000),
  writer_queue_capacity (4096), max_tags_per_entry (16), max_tag_bytes (64), purge_guard_window_s (300)
- `[persistence]`: enabled (true), dir ("data"), snapshot_interval_s (300), fsync_interval_ms (1000),
  retry_interval_ms (30000), max_wal_bytes (256 MiB)
- `[index]`: kind (`auto` | `brute` | `hnsw`), hnsw_min_partition_size (5000), hnsw_m (16),
  hnsw_ef_construction (200), hnsw_ef_search (64), hnsw_max_tombstone_ratio (0.25), hnsw_seed (42)
- `[coalescer]`: enabled (true), max_waiters_per_flight (1000)
- `[limits]`: rps (50), burst (100) per tenant
- `[logging]`: level, log_prompts (false)
- `[[tenants]]`: id, key_env, optional max_bytes, optional ttl_s

Env override example: `SEMCACHE_CACHE__SIMILARITY_THRESHOLD=0.85`. Secrets:
`SEMCACHE_UPSTREAM_API_KEY`, `SEMCACHE_ADMIN_KEY` (enables `/admin/purge`) and the variables named
by `key_env`.

---

## 19. Observability: logs, metrics, endpoints

**Log line per request** (JSON): `ts, level, msg="request", req_id, tenant, status, cache,
streamed, latency_ms, upstream_ms, upstream_error, bytes_in, [similarity], [bypass_reason],
[store]`. No prompts, no keys.

**Main metrics** (`/metrics`):
- requests: `semcache_requests_total{result}`, `semcache_request_seconds`, `semcache_inflight`
- rejections: `semcache_bad_requests_total`, `semcache_auth_failures_total`,
  `semcache_rejected_total{reason="rate_limit|overload"}`, `semcache_client_disconnects_total`
- upstream: `semcache_upstream_seconds`, `semcache_upstream_errors_total{kind}`,
  `semcache_upstream_responses_total{class}`
- cache: `semcache_bypass_total{reason}`, `semcache_lookup_seconds`, `semcache_embed_seconds`,
  `semcache_best_similarity` (histogram for threshold tuning), `semcache_tokens_saved_total`,
  `semcache_embed_errors_total`, `semcache_embed_queue_rejected_total`,
  `semcache_cache_internal_errors_total`, `semcache_store_skipped_total{reason}`,
  `semcache_index_entries`, `semcache_index_bytes`, `semcache_partitions`,
  `semcache_evictions_total{reason}`, `semcache_embedder_ready`
- coalescing / writer: `semcache_requests_total{result="COALESCED"}`, `semcache_flights_active`,
  `semcache_coalesce_independent_total`, `semcache_writer_queue_depth`, `semcache_store_dropped_total`
- index: `semcache_index_hnsw_partitions`, `semcache_index_rebuilds_total`,
  `semcache_index_rebuild_seconds`
- persistence: `semcache_persistence_enabled`, `semcache_wal_bytes`, `semcache_snapshots_total`,
  `semcache_snapshot_seconds`, `semcache_persistence_errors_total`,
  `semcache_persistence_recovery_total{result}`, `semcache_evictions_total{reason="purge"}`

**Endpoints:** `/healthz`, `/readyz`, `/stats`, `/metrics`, `POST /admin/purge` (admin key).

---

## 20. Testing: how we know it works

**221 tests**, run under three builds (debug, ASan+UBSan, TSan).

**Unit tests (`tests/unit/`):**

| File | Covers |
|---|---|
| `config_test.cpp` | defaults, file values, env overrides, secrets only from env, unknown keys rejected, type errors, validation, embedder/cache sections |
| `log_test.cpp` | one JSON object per line with `ts/level/msg` first; invalid UTF-8 replaced, not thrown |
| `metrics_test.cpp` | counters and gauges, same name with two types rejected, histogram buckets and quantiles, Prometheus rendering |
| `rate_limiter_test.cpp` | burst then refill over time, keys independent (injected clock) |
| `tenants_test.cpp` | bearer key → tenant; malformed/unknown headers rejected |
| `mock_response_test.cpp` | mock is deterministic, depends on prompt and model, stream events reassemble to the content, fault-string parsing |
| `bert_normalizer_test.cpp` | examples + **every code point** vs HuggingFace (272 blocks) |
| `tokenizer_test.cpp` | WordPiece rules on a tiny vocab + **3,125 golden strings** exact |
| `embedder_test.cpp` | **cosine > 0.999 gate** on 125 sentences, batch = single, unit length, paraphrase ordering, too-long rejection, concurrency, missing/corrupt model |
| `context_key_test.cpp` | every parameter changes the key, absent ≠ default, transport fields don't, key order/number spelling don't, system messages, history window K, tool calls, field boundaries, NFC, all bypass reasons |
| `simd_test.cpp` | AVX2 = scalar for all tail lengths, unaligned pointers, row scoring |
| `vector_index_test.cpp` | interface contract (empty, exact find, sorted top-k, remove, replace, wrong dimension, memory), run against brute force (scalar, AVX2) **and HNSW** (default and small-M) |
| `hnsw_test.cpp` | **recall@10 ≥ 0.95 vs brute force**, larger ef never lowers recall, tombstones never returned + rebuild signal, same seed → same graph, memory bound, concurrent searches agree (TSan) |
| `cache_store_test.cpp` | exact/nearest isolation, exact-only entries, TTL lazy + sweep, per-tenant TTL, replace, CLOCK second chance, tenant quota, too-large, evicted entry still valid for its reader, **4 readers + 1 writer** (TSan), auto upgrade to HNSW, tombstone rebuild (deduplicated), readers during upgrades |
| `cache_writer_test.cpp` | inserts applied asynchronously in order, callback always runs, full queue drops + counts, stop applies queued work, expiry on the writer |
| `coalescer_test.cpp` | leader/follower roles and retire, complete replay with own headers, live stream replay, leader gone before head → serve alone, mid-stream abort → error event, follower cap, leader keeps going for followers, bounded log |
| `persistence_test.cpp` | formats and fault injection on real files (§14.8) |
| `worker_pool_test.cpp` | runs on pool thread and resumes on caller, exceptions, full queue rejects immediately, stop completes every caller |
| `completion_codec_test.cpp` | reassembly at random chunk boundaries, tool-call merging, incomplete/error streams, overflow, storability rules, fresh ids, **re-stream round trip** |

**Integration tests (`tests/integration/`)** — real sockets, real proxy, mock upstream:

| File | Covers |
|---|---|
| `passthrough_test.cpp` | bodies relayed verbatim; client key replaced by the upstream key (and none sent if unset); header allow-list; two tenants; 401 missing/unknown key; 400 invalid JSON (not forwarded); 413 oversized body; 404/405; per-tenant 429; upstream path prefix; `/healthz`; `/readyz` ready and degraded; `/metrics` counts |
| `streaming_test.cpp` | events arrive incrementally and reassemble; usage chunk relayed; several streams on one keep-alive connection |
| `failure_test.cpp` | 5xx passthrough, unreachable → 502, first-byte/total/idle timeouts, reset mid-stream, oversized response, overload → 503, shutdown cancels hung upstream calls |
| `cache_test.cpp` | MISS then exact HIT without upstream call; paraphrase above/below threshold; configurable threshold; streamed miss served to both modes; tenants isolated; different context misses; bypass header; uncacheable requests; cache disabled; errors and interrupted streams never stored; embedder failure fails open; too-long prompt exact-only; no-embedder mode; full embed queue fails open; `/stats` and metrics |
| `semantic_model_test.cpp` | the **real model** end to end: rewordings HIT, different questions MISS |
| `restart_purge_test.cpp` | restart keeps HITs, purge by tag/tenant, purge durable, in-flight guard, admin auth, invalid tags (§14.8) |
| `herd_test.cpp` | coalescing under herds (§12.8): 50 → 1 upstream call, live streaming followers, shared errors, cap, isolation, leader hang-up, mixed-traffic stress |

**Test helpers:** `TestEnv` starts mock + proxy on random ports in-process and drains them on
destruction; `FakeEmbedder` makes similarities exact (`alias(text, canonical, 0.95)`), can fail on
demand, and can block (to fill the queue).

**Mutation checks** (break the code on purpose, confirm tests fail): removing the tenant from the
context key (unit + cross-tenant integration test fail); disabling coalescing (8 of 12 herd tests
fail); letting a leader stop when its client leaves (unit test + a dedicated integration test fail);
disabling the purge guard, or not logging removals to the WAL (a unit and an integration test fail
for each).

---

## 21. Measurements so far

All from reproducible scripts; raw JSON under `bench/results/`. Machine: i7-1165G7 laptop under
WSL2, which measured well below the CPU's capability (~35 GFLOP/s single-core).

**Embedding (Phase 2):**

| Prompt length | One embed |
|---|---:|
| 16 tokens | 11 ms |
| 32 tokens | 22 ms |
| 128 tokens | 82 ms |
| 256 tokens | 168 ms |

Throughput: 54/s on 1 thread, 87/s on 4, 107/s on 8. Batching does not help on CPU. Tokenizing is
< 1% of the time. The int8 model would be 2–3× faster but only reaches cosine 0.9725 (fails the
gate) — not adopted.

**Search (Phase 3):**

| Vectors | AVX2 | Scalar |
|---:|---:|---:|
| 1,000 | 69 µs | 699 µs |
| 10,000 | 1.3 ms | 6.8 ms |
| 20,000 | 2.8 ms | 15.6 ms |
| 100,000 | 16.2 ms | 67.8 ms |

→ brute force stays within the 2 ms lookup budget up to ~15K vectors per partition.

**End to end (real binary + real model, smoke test):** exact HIT ~0.6 ms added; semantic HIT
~10 ms added (mostly embedding); MISS = upstream time (mock: 400 ms).

**Coalescing (Phase 4, real binary):** 30 concurrent identical requests → 1 upstream call
(1 MISS + 29 COALESCED, one distinct body); 10 identical streams → 1 upstream call, all complete.

**HNSW (Phase 5):** see the table in §13.5 — recall@10 = 1.0 at ef=64 on synthetic clustered
data up to 50K vectors; p50 224 µs vs 1,451 µs brute force at 10K; auto switch at 5,000 vectors.

**Evaluation (Phase 7):** see §15 — threshold 0.97 (QQP recall 12.4%, false-hit 0.7%); exact HIT
24.5K req/s at p50 1 ms; semantic paths embedding-bound (~300 req/s); herd 50×20: 20 vs 1,000
upstream calls; mixed workload −81% upstream calls.

**Persistence (Phase 6):** see §14.7 — 20K entries recover in 0.26 s with brute-force partitions,
~17 s as one HNSW partition; ~3.7 KB on disk per entry.

---

## 22. Known weaknesses and what comes next

**Known weaknesses:**
- Embedding is slow on this laptop: above the 10 ms HIT budget for longer prompts. Re-measure on
  target hardware; levers are the exact fast path, coalescing, and possibly int8 (Phase 7).
- The model misses real paraphrases with different wording and is fooled by negation; at the safe
  threshold (0.97) it catches only 12% of QQP duplicates (§15.2). A stronger embedding model or a
  re-ranker is the lever.
- Semantic HITs and MISSes are embedding-bound (~300 req/s on this laptop); the 10 ms HIT target
  holds for exact HITs only here.
- Context key K=2 can share answers between conversations that differ only earlier (ADR-002).
- HNSW recall was measured on synthetic clusters; real embeddings (Phase 7) may need a larger `ef`.
- HNSW builds grow super-linearly (100 s at 50K on this laptop) and run on the single writer
  thread; inserts queue (and may be dropped) during an upgrade or tombstone rebuild.
- Coalescing is exact-key only; herds of *paraphrases* each go upstream (ADR-009).
- Followers receive the leader's response id (by design: identical response).
- Startup recovery of a large HNSW partition takes ~17 s per 20K vectors here (graph rebuilt from
  vectors), and the server only starts listening afterwards.
- Purge by tag scans all entries (fine at the design's ~200K entries; no tag index).
- Removing files when persistence is disabled is best effort; if that also fails, a crash before
  the retry succeeds could bring back entries purged meanwhile.
- Streamed misses without `include_usage` store no usage → tokens saved is undercounted.
- `/stats` percentiles are bucket estimates.
- CI runs on GitHub for every push (debug, release, ASan, TSan); Phase 6 was not yet pushed when this was written.

**Next phases:**
8. Dashboard.
9. Docker, deploy docs, README.

---

## 23. How to build, run and test

```bash
# one-time setup
scripts/bootstrap-vcpkg.sh           # vcpkg at the pinned commit
scripts/fetch-model.sh               # model + vocab, hash-checked

# build (ALWAYS -j4 on this machine)
cmake --preset debug && cmake --build build/debug -j4
ctest --test-dir build/debug         # 221 tests

cmake --preset asan && cmake --build build/asan -j4 && ctest --test-dir build/asan
cmake --preset tsan && cmake --build build/tsan -j4 && ctest --test-dir build/tsan

# run locally
./build/debug/semcache-mock --port 9090 &
SEMCACHE_KEY_DEV=sk-sc-dev ./build/debug/semcache --config config/dev.toml
curl -s -i -H "Authorization: Bearer sk-sc-dev" \
  -d '{"model":"m","messages":[{"role":"user","content":"How do I reset my password?"}]}' \
  localhost:8080/v1/chat/completions          # MISS, then run again: HIT
curl -s localhost:8080/stats

# benchmarks
cmake --preset release && cmake --build build/release -j4
bench/run_embed_bench.sh
./build/release/semcache_bench_index --benchmark_repetitions=5
./build/release/semcache_hnsw_eval --sizes 1000,5000,10000 --queries 500 --out hnsw.json
./build/release/semcache_persist_eval --entries 20000 --partitions 200

# Phase 7 evaluation (see bench/results/phase7/README.md for the full list)
.venv/bin/python tools/prepare_eval.py
./build/release/semcache_embed_texts --in data/eval/qqp_texts.jsonl --out data/eval/qqp
./build/release/semcache_embed_texts --in data/eval/faq_texts.jsonl --out data/eval/faq
.venv/bin/python tools/analyze_threshold.py
.venv/bin/python tools/run_load_eval.py
.venv/bin/python tools/cost_model.py

# purge (needs SEMCACHE_ADMIN_KEY set when the server started)
curl -s -H "Authorization: Bearer $SEMCACHE_ADMIN_KEY" -d '{"tag":"faq"}' localhost:8080/admin/purge

# Python tooling (only to regenerate golden files / tables)
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
.venv/bin/python tools/make_golden.py
.venv/bin/python tools/gen_bert_tables.py
```

---

## 24. Glossary

| Term | Meaning |
|---|---|
| **Embedding** | A fixed-length list of numbers (here 384) representing a text's meaning. |
| **Cosine similarity** | How aligned two vectors are (1 = same direction). For unit-length vectors it equals the dot product. |
| **Context key** | SHA-256 of everything except the final question; must match exactly. |
| **Partition** | All cache entries with the same context key. |
| **Exact fast path** | Same normalized final question → HIT without running the model. |
| **HIT / MISS / BYPASS / COALESCED** | How a request was served (see §1). |
| **SSE** | Server-Sent Events: `data: …\n\n` lines streamed over HTTP; how LLM APIs stream tokens. |
| **Chunked encoding** | HTTP/1.1 way to send a body of unknown length piece by piece. |
| **Token / WordPiece** | The model's units of text; WordPiece splits unknown words into known sub-pieces (`##`). |
| **NFD / NFC** | Unicode decomposed / composed forms (`é` as one char vs `e` + accent). |
| **ONNX / ONNX Runtime** | A file format for neural networks / the engine that runs them. |
| **Mean pooling** | Averaging per-token vectors into one sentence vector. |
| **SIMD / AVX2 / FMA** | CPU instructions that process several numbers at once / Intel's 256-bit vector set / fused multiply-add. |
| **Brute force / HNSW** | Compare against every vector (exact) / a graph index that is approximate but much faster at scale. |
| **TTL** | Time to live: how long an entry stays valid. |
| **CLOCK** | Approximate least-recently-used eviction using a "referenced" bit and a rotating hand. |
| **Coroutine** | A function that can suspend at `co_await` and resume later. |
| **Strand** | Asio executor that runs handlers one at a time. |
| **Fail open** | When the cache breaks, still serve the request via upstream. |
| **ADR** | Architecture Decision Record. |
| **Golden file** | Reference outputs from a trusted implementation that tests compare against. |
| **Sanitizers** | ASan (memory errors), UBSan (undefined behaviour), TSan (data races), LSan (leaks). |
| **Mutation test** | Breaking the code on purpose to check the tests notice. |
| **Flight / leader / follower** | One shared upstream call / the request that makes it / the identical requests that wait for and replay its response. |
| **Writer thread** | The one thread allowed to change the cache; everyone else submits work to its queue. |
| **Recall@k** | Share of the true k nearest neighbours that an approximate search returns (1.0 = all). |
| **M / ef** | HNSW links per node / beam width (how many candidates a search keeps). |
| **Recall / precision / false-hit rate** | Share of true duplicates that hit / share of hits that were correct / share of look-alike non-duplicates that (wrongly) hit. |
| **AUC** | Probability that a random duplicate pair scores higher than a random non-duplicate pair (1.0 = perfect separation). |
| **Hard negative** | A question worded like another but needing a different answer. |
| **Closed-loop load test** | Each simulated client sends its next request as soon as the previous one returns. |
| **WAL / LSN** | Write-ahead log: an append-only record of every change / its sequence number. |
| **Snapshot** | A full copy of the cache at one LSN; the WAL after it brings recovery up to date. |
| **fsync / fdatasync** | Ask the OS to actually put written bytes on the disk (not just in its cache). |
| **Torn write** | A record only partly written when the process or machine died. |
| **CRC-32C** | A checksum that detects corrupted or torn records. |
| **Purge guard** | Drops answers that were being computed while a matching purge ran. |
| **Tombstone** | A deleted item kept in place (marked dead) because removing it outright is expensive or unsafe. |
