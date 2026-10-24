# Demo script

A 3–5 minute walkthrough of the dashboard for someone who has never heard of semantic caching.
Everything runs locally against the mock LLM (`semcache-mock`), so it costs nothing and works
offline.

## 1. Start it

```bash
scripts/fetch-model.sh                                 # once: model + vocab, hash-checked
cmake --preset release && cmake --build build/release -j4
./build/release/semcache-mock --port 9090 &            # fake LLM: 800 ms per answer, deterministic
SEMCACHE_KEY_DEV=sk-sc-dev ./build/release/semcache --config config/dev.toml
```

Open <http://127.0.0.1:8080/dashboard>. The pills at the top should read **demo mode (mock LLM)**,
**embedder ready** and **similarity threshold 0.97**. If the embedder pill says *exact matches
only*, the model files are missing (`scripts/fetch-model.sh`); scenario 2 will then miss.

`config/dev.toml` runs in demo mode. Demo mode refuses to start against anything except the mock
(see §5), so this setup cannot spend money by accident.

## 2. The 10-second pitch (top of the page)

> "Apps ask LLMs the same questions over and over. semcache sits in front of the API. If it has
> seen the question before, or one that means the same, it answers from memory in a few
> milliseconds instead of paying for a new answer. The tiles show it live: how many requests were
> answered from the cache, how many LLM calls that saved, and how fast each path is."

Point at **Typical answer time**: cache vs upstream (on the dev laptop, one run: ~1.5 ms vs
~415 ms with the mock's latency set to 400 ms; the mock defaults to 800 ms).

## 3. The four scenario cards

Each **Run** uses a fresh conversation (a new system message), so every run starts cold, and
the cards can be re-run during the talk.

1. **Repeat.** "Same question twice. The first one goes to the LLM (MISS, ~0.4–0.8 s), the second
   comes from the cache (HIT, a few ms)." The verdict line states both times.
2. **Reword.** "*How do I cancel my subscription?* then *How can I cancel my subscription?*
   Different text, same meaning. The cache compares meaning (an embedding), not characters."
   Shows HIT with similarity 0.996.
3. **The trap.** Run this one deliberately. "*What does the warranty cover?* then *What does the
   warranty **not** cover?* The model rates these 0.974 similar, above the 0.97 threshold, so the
   second question gets the first one's answer. That is wrong, and the page says so in red."
   Follow-up: the threshold was chosen from measured data (Phase 7, ADR-012: at most 1% false
   hits on look-alike Quora question pairs), negation is a known blind spot of this small model,
   and apps where that matters should cache exact repeats only or bypass the cache per request
   (`X-Semcache-Bypass: 1`).
4. **Rush.** "Several users ask the same *new* question at the same moment" (8 requests with the
   default per-address burst of 10; the card shows the number). Exactly one request
   reaches the LLM (one MISS). The others either wait for that call and share its answer
   (COALESCED) or arrive after it was stored (HIT). A browser opens at most 6 connections per
   site, so the rest queue in the browser and show up as HITs. The load test in
   `bench/results/phase7` shows the pure case: 50 identical requests at once, 20 rounds, made 20
   upstream calls with coalescing on and 1,000 with it off.

## 4. Try it, recent requests, reset

- **Try it:** type any question, send it twice. The second is a HIT. Tick/untick *stream the
  answer* to show that cached answers are re-streamed token by token like a live answer. The
  list under it is kept only in this browser tab.
- **Recent requests:** the server's view. It has results, status, latency and similarity, and
  never the prompt text: `/stats` does not contain prompts or tenant ids, by construction.
- **Cache hit rate over time:** the cumulative hit rate since the proxy started.
- **Reset demo cache** (footer) empties the demo tenant's cache so the talk can start over. It
  purges tenant `demo` only.

## 5. What demo mode enforces (if someone asks "can I break it?")

| Protection | Setting | What happens |
|---|---|---|
| Mock upstream only | `demo.allowed_upstreams`, no `SEMCACHE_UPSTREAM_API_KEY` | refuses to start otherwise |
| Public key, own tenant | `demo.public_key` → tenant `demo` | shown in `/stats`; other tenants are unaffected |
| Per-address rate limit | `demo.per_ip_rps = 2`, `per_ip_burst = 10` | 429 + `Retry-After` |
| Bounded limiter memory | `demo.max_tracked_ips = 10000` | idle addresses dropped first, then least recently used |
| Prompt length | `demo.max_prompt_chars = 500` | 400 `prompt_too_long`, never reaches cache or LLM |
| Cache memory | `demo.max_bytes = 64 MiB` | caps `cache.max_bytes` |
| Live-stats connections | `stats.max_stream_clients = 32` | 503 + `Retry-After` |
| Page | CSP `default-src 'self'`, no inline script, `textContent` only | no third-party code, no injected HTML |

Known limits: behind a reverse proxy all visitors share one address (forwarded headers are not
trusted), and an attacker rotating many addresses can push a busy address out of the limiter.

## 6. Troubleshooting

- **Rush shows 429s:** the per-address burst (10) is shared by everything this browser sends;
  the card sizes the rush to `per_ip_burst - 2` (8 by default). Wait a few seconds and rerun.
- **Reword shows MISS:** embedder not loaded (see the pill), or a non-default threshold.
- **Tiles say "reconnecting…":** the proxy stopped; the page reconnects by itself.

## 7. Automated check of this script

`tools/dashboard_e2e.py` performs every step above in headless Chromium against the real binaries
(with the CSP enforced) and checks the page against `/stats` and the mock's call counter:

```bash
.venv/bin/pip install -r tools/requirements.txt
.venv/bin/python -m playwright install chromium
.venv/bin/python tools/dashboard_e2e.py --bin build/release [--screenshot docs/img/dashboard.png]
```

Without root, Chromium's missing system libraries (`libnss3`, `libnspr4`, `libasound2t64` on
Ubuntu 24.04) can be unpacked into a user directory:

```bash
mkdir -p ~/.cache/semcache-browser-libs/debs && cd ~/.cache/semcache-browser-libs/debs
apt-get download libnss3 libnspr4 libasound2t64      # if a version 404s: apt-get download libnss3=<older>
for d in *.deb; do dpkg-deb -x "$d" ../root; done
export LD_LIBRARY_PATH=~/.cache/semcache-browser-libs/root/usr/lib/x86_64-linux-gnu
```

![dashboard](img/dashboard.png)
