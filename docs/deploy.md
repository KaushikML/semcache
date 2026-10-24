# Deploying semcache

One container image with two binaries: `semcache` (the proxy) and `semcache-mock` (the fake LLM
used for demos and tests). This page covers building it, running the demo, running it in front
of a real API, sizing, persistence, monitoring, shutdown and security. Design background:
[`design.md`](design.md) (§7 concurrency, §8 failure modes, §10 security, §11 scaling).

## 1. The image

```bash
docker build -t semcache:local .            # or: docker compose build
docker build --build-arg JOBS=8 -t semcache:local .   # more compile jobs if you have the RAM
```

- **Build stage** (Ubuntu 24.04, g++ 13): vcpkg at the baseline pinned in `vcpkg.json`, ONNX
  Runtime 1.20.1 (SHA-256 pinned in `cmake/onnxruntime.cmake`), all-MiniLM-L6-v2 at a pinned
  revision (SHA-256 checked by `scripts/fetch-model.sh`). Dependencies are a separate layer, so
  source edits do not rebuild them. The first build compiles Boost, OpenSSL and the rest from
  source and takes a while; `JOBS` (default 4) caps parallel compile jobs to bound memory use.
- **Runtime stage** (Ubuntu 24.04 + `ca-certificates`; the image is 343 MB), layout:

  | Path | What |
  |---|---|
  | `/opt/semcache/bin/semcache`, `semcache-mock` | stripped binaries (`cmake --install --strip`) |
  | `/opt/semcache/lib/libonnxruntime.so.1` | found through `RUNPATH $ORIGIN/../lib` |
  | `/opt/semcache/models/` | model + vocab (baked in: no download at start, works offline) |
  | `/etc/semcache/semcache.toml` | default config: live mode (`config/docker.toml`) |
  | `/etc/semcache/demo.toml` | demo config (`config/docker-demo.toml`) |
  | `/var/lib/semcache/data` | persistence directory: mount a volume here |

- Runs as user `semcache` (uid 10001), listens on 8080, has a `HEALTHCHECK` on `/healthz`
  (bash `/dev/tcp`; there is no curl in the image). SIGTERM triggers a graceful drain.
- The binary picks AVX2 or scalar code at runtime, so the image runs on any x86-64 CPU. There is
  no arm64 build: ONNX Runtime is pinned only for linux-x64 (ADR-003).

Without Docker, `cmake --install build/release --prefix /opt/semcache --strip` produces the same
tree; copy `models/` next to it and point `embedder.model_path` at it.

## 2. Demo (`docker compose up`)

`docker-compose.yml` starts the mock LLM and semcache in demo mode. Open
<http://127.0.0.1:8080/dashboard> and follow [`demo-script.md`](demo-script.md).

- The port is published on **127.0.0.1 only**. Before exposing a demo publicly, read §8: demo
  mode is built for it (mock-only upstream, public key, per-address limits, prompt and memory
  caps), but put TLS and a reverse proxy in front, and note the per-address limit caveat there.
- The cache lives in the named volume `semcache-data` and survives restarts;
  `docker compose down -v` deletes it.
- Containers run with a read-only root filesystem, all capabilities dropped and
  `no-new-privileges`.
- `scripts/docker-smoke.sh` checks the whole stack (also run in CI): health, dashboard + CSP,
  MISS → HIT, cache survives a restart, prompt text absent from logs, clean exit on SIGTERM.

## 3. In front of a real API

The default config (`config/docker.toml`) is live mode against `https://api.openai.com` with one
tenant called `default`:

```bash
export SEMCACHE_UPSTREAM_API_KEY=sk-...                          # your provider key
export SEMCACHE_KEY_DEFAULT="sk-sc-$(openssl rand -hex 24)"      # the key YOUR clients will send
docker run -d --name semcache -p 127.0.0.1:8080:8080 \
  -e SEMCACHE_UPSTREAM_API_KEY -e SEMCACHE_KEY_DEFAULT \
  -v semcache-data:/var/lib/semcache/data \
  --read-only --cap-drop ALL --security-opt no-new-privileges \
  --stop-timeout 15 semcache:local
```

Clients change only their base URL and key:

```python
client = OpenAI(base_url="http://semcache.internal:8080/v1", api_key=os.environ["SEMCACHE_KEY_DEFAULT"])
```

- **Keys:** clients never see the provider key. semcache issues its own keys per tenant (ADR-001)
  and keeps only their SHA-256. A missing env var for a configured tenant stops startup with a
  clear error.
- **Several tenants:** mount your own config over `/etc/semcache/semcache.toml` with one
  `[[tenants]]` block per tenant (`id`, `key_env`, optional `max_bytes` quota and `ttl_s`), and
  pass each `key_env` variable. Tenants never share cache entries (the tenant id is part of the
  context key).
- **Other OpenAI-compatible providers:** set `SEMCACHE_UPSTREAM__BASE_URL` (e.g. an Azure,
  vLLM or LiteLLM endpoint; a path prefix is kept).
- **Purge:** set `SEMCACHE_ADMIN_KEY` to enable `POST /admin/purge` (by tag, tenant or context;
  see `design.md` §6). Without it, `/admin/*` returns 404.
- **Per-request control:** clients can send `X-Semcache-Bypass: 1` to skip the cache, and
  `X-Semcache-Tags: a,b` to tag the stored answer for later purge.

### Configuration

Every key in [`config/semcache.example.toml`](../config/semcache.example.toml) (all keys, with
comments) can be set by env var: `SEMCACHE_<SECTION>__<KEY>`, e.g.

| Env var | Default | When to change it |
|---|---|---|
| `SEMCACHE_CACHE__SIMILARITY_THRESHOLD` | 0.97 | Lower = more semantic hits **and** more wrong answers; see the curve in `bench/results/phase7` |
| `SEMCACHE_CACHE__MAX_BYTES` | 1 GiB | Cache budget; size the container from §4 |
| `SEMCACHE_CACHE__DEFAULT_TTL_S` | 86400 | How long an answer may be reused |
| `SEMCACHE_CACHE__HISTORY_K` | 2 | Earlier turns in the context key; `-1` = whole conversation (safest) |
| `SEMCACHE_CACHE__ENABLED` | true | `false` = pure passthrough proxy |
| `SEMCACHE_EMBEDDER__THREADS` | 4 | Embedding pool; about one per core you give semantic lookups |
| `SEMCACHE_LIMITS__RPS` / `__BURST` | 50 / 100 | Per-tenant rate limit |
| `SEMCACHE_SERVER__MAX_INFLIGHT` | 1024 | Concurrent chat requests before 503 |
| `SEMCACHE_LOGGING__LEVEL` | info | `warn` under heavy load (one line per request at info) |

Unknown keys and invalid values stop startup with a message naming the key.

## 4. Sizing

Measured with the installed (stripped) binaries by `tools/run_memory_eval.py`
([`bench/results/phase9/memory.md`](../bench/results/phase9/memory.md)); one laptop, so treat
them as indicative:

| | Measured (10,000 entries, ~2 KB answers, one HNSW partition) |
|---|---|
| Idle, model loaded | **167 MiB** RSS, ready in 0.4 s |
| After storing 10,000 answers | 247 MiB RSS; the cache accounted 40.8 MiB (~4.2 KB per entry) |
| Restart with 10,000 entries | ready in **7.9 s** (HNSW graph rebuilt), 222 MiB RSS |
| On disk | 38.6 MiB (~4 KB per entry) |

- **Memory ≈ baseline + about 2 × the accounted cache bytes.** In this run RSS grew by ~80 MiB
  for 40.8 MiB of accounted entries (~8 KB per entry): the HNSW graph, hash tables and allocator
  overhead are not counted in `cache.max_bytes`. A starting point for the container limit:
  `200 MiB + 2 × cache.max_bytes`, then watch RSS against `semcache_index_bytes`. With the default
  1 GiB budget that is ~2.2 GiB; lower `SEMCACHE_CACHE__MAX_BYTES` for small containers.
- **CPU:** exact HITs cost ~1 ms through the full stack (24.5K req/s on 6 shared vCPUs). Every
  non-exact request pays one embedding: ~13 ms of CPU for a short prompt on the dev laptop, up to
  ~170 ms at 256 tokens. That decides the core count: semantic throughput ≈ embed threads ÷
  embed time. Re-measure on your hardware with `bench/run_embed_bench.sh`.
- **Startup:** the server listens only after recovery. A large single HNSW partition is rebuilt
  from its vectors on start (7.9 s for 10K and ~17 s for 20K vectors on the dev laptop);
  brute-force partitions (< 5,000 entries each) load in well under a second. Allow for it in
  probe start periods (the image's `HEALTHCHECK` allows 30 s).
- **Disk:** ~3.7 KB per entry at ~2 KB answers (snapshot + WAL); the WAL is compacted into the
  snapshot every `persistence.snapshot_interval_s` or at `max_wal_bytes`.

## 5. Persistence and upgrades

- Inserts are written to the WAL and fsynced every `persistence.fsync_interval_ms` (1 s): a
  crash loses at most that window. Snapshots every 5 minutes; recovery truncates a torn tail and
  sets corrupt files aside (`*.corrupt-<time>`) instead of failing.
- **Disk full or read-only:** the cache keeps serving from memory, counts
  `semcache_persistence_errors_total` and retries every `retry_interval_ms`. Requests are never
  failed because of the disk.
- **Upgrading the image** keeps the cache. **Changing the model** (a different file) is detected
  by its SHA-256 fingerprint: stored vectors are dropped and old entries are served by exact match
  only until they expire.
- **Backup:** copy the data directory while the container is stopped, or copy `snapshot.bin`
  right after a snapshot. The cache is a cache: losing it costs upstream calls, not data.
- To start empty: stop, delete the volume (`docker volume rm semcache-data`), start.

## 6. Health, metrics, logs

| Endpoint | Use |
|---|---|
| `GET /healthz` | Liveness: the process serves HTTP. |
| `GET /readyz` | Readiness. `ready`, or `degraded` with `["embedder"]` if the model failed to load. **Both are 200**: a degraded node still serves every request (passthrough + exact hits), so taking it out of rotation would turn a cache problem into an outage. Alert on it instead. |
| `GET /metrics` | Prometheus text format. |
| `GET /stats`, `/stats/stream` | JSON (and SSE) summary for the dashboard. No prompts, no tenant ids. |

Prometheus scrape config:

```yaml
scrape_configs:
  - job_name: semcache
    static_configs: [{ targets: ["semcache.internal:8080"] }]
```

Alerts worth having:

| Expression | Meaning |
|---|---|
| `semcache_embedder_ready == 0` | Semantic matching is off (exact hits and passthrough still work). |
| `rate(semcache_persistence_errors_total[5m]) > 0` | Disk problems; running memory-only. |
| `rate(semcache_upstream_errors_total[5m]) > 0` | Upstream unreachable or timing out (by `kind`). |
| `rate(semcache_rejected_total{reason="overload"}[5m]) > 0` | `max_inflight` reached: add capacity. |
| `rate(semcache_store_dropped_total[5m]) > 0` | Writer queue full: inserts are being dropped. |
| `rate(semcache_embed_queue_rejected_total[5m]) > 0` | Embed pool saturated: requests bypass the cache. |

Hit rate: `sum(rate(semcache_requests_total{result="HIT"}[5m])) / sum(rate(semcache_requests_total[5m]))`.
Threshold tuning: `semcache_best_similarity` is a histogram of the best score per lookup.

Logs are JSON lines on stdout, one per request (`req_id`, `tenant`, `status`, `cache`,
`latency_ms`, `upstream_ms`, `similarity`, …). Prompt and answer text are never logged unless
`logging.log_prompts = true`. Keys are never logged.

## 7. Shutdown, restarts, scaling

- On SIGTERM semcache stops accepting, lets in-flight requests finish for up to
  `server.shutdown_timeout_ms` (5 s), flushes the WAL and exits 0. Give the orchestrator a
  longer grace period (compose: `stop_grace_period: 15s`; Kubernetes:
  `terminationGracePeriodSeconds: 15`).
- **One node** is what is implemented. Several replicas behind a load balancer work, but each has
  its own cache and coalesces only its own traffic, so hit rate drops roughly with the replica
  count. Routing by context key (consistent hashing; `design.md` §11) fixes that; it is designed,
  not built.
- Kubernetes mapping: liveness `/healthz`, readiness `/readyz` (see above: it does not fail on
  `degraded`), a `startupProbe` sized for recovery time (§4), a PersistentVolume at
  `/var/lib/semcache/data`, `readOnlyRootFilesystem: true`, `runAsNonRoot: true`.

## 8. Network and security checklist

- **TLS:** semcache speaks plain HTTP. Terminate TLS in a reverse proxy (nginx, Caddy, a cloud
  load balancer) or keep it on a private network. For streaming, disable response buffering
  (nginx: `proxy_buffering off;`) so tokens are not held back.
- **Expose only what clients need:** `/v1/*` (and `/dashboard`, `/stats*` for a public demo).
  Keep `/metrics`, `/admin/*` and, in live mode, `/stats*` on the internal network: they reveal
  no prompts, but they do reveal traffic volume and timing.
- **Secrets** only as env vars (or files your orchestrator turns into env vars); never in the
  TOML. Rotate a client key by changing its env var and restarting.
- **Public demo:** the per-address limit sees the TCP peer address. Behind a reverse proxy every
  visitor shares the proxy's address (forwarded headers are not trusted, so they cannot be
  spoofed either). Rate-limit per client at the reverse proxy in that case.
- **Data:** cached answers are stored unencrypted in the data directory; protect the volume like
  the API traffic itself. Purge a tenant with `POST /admin/purge {"tenant": "<id>"}`.
- The cache never fails a request upstream would have served (fail open). If a wrong cached answer
  is costlier than an upstream call for some traffic, send `X-Semcache-Bypass: 1` for it.

## 9. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| exits with `config error: tenants.default: env var SEMCACHE_KEY_DEFAULT is not set` | pass the tenant key env var (§3) |
| `demo mode: upstream … is not in demo.allowed_upstreams` | demo mode only talks to the mock; use live mode for a real API |
| every response `X-Semcache: BYPASS` with `bypass_reason` in the log | request not cacheable (n > 1, temperature above `max_temperature`, bypass header…) or the embedder is overloaded |
| `/readyz` says `degraded` | model files missing or unreadable at `embedder.model_path` |
| upstream TLS errors | outbound HTTPS blocked, or a custom CA: set `SSL_CERT_FILE` to a bundle that includes it |
| streams arrive all at once | a proxy in front is buffering (§8) |
