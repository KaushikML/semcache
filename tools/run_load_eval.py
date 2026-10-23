#!/usr/bin/env python3
"""End-to-end load scenarios against the real binaries (Phase 7).

Starts semcache-mock and semcache (release build) per scenario, drives them with the C++ load
generator (build/release/semcache_loadgen; all timing is measured there, not in Python), and
writes bench/results/phase7/load_<scenario>.json with the loadgen summary, /stats, and the
number of upstream calls the mock received.

Scenarios: exact_hit, semantic_hit, miss, mixed (+ mixed_nocache), herd (coalescing on/off),
index (one 10K-entry partition, brute force vs HNSW).
Usage: .venv/bin/python tools/run_load_eval.py [--only name,...] [--quick]
"""

import argparse
import json
import os
import pathlib
import subprocess
import tempfile
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build/release"
OUT = ROOT / "bench/results/phase7"
EVAL = ROOT / "data/eval"
PROXY_PORT, MOCK_PORT = 18080, 19090
KEY = "sk-sc-load"

CONFIG = f"""
mode = "live"
[server]
host = "127.0.0.1"
port = {PROXY_PORT}
max_inflight = 4096
[upstream]
base_url = "http://127.0.0.1:{MOCK_PORT}"
[limits]
rps = 1000000
burst = 1000000
[persistence]
enabled = false
[logging]
level = "warn"
[[tenants]]
id = "load"
key_env = "SEMCACHE_KEY_LOAD"
"""


def get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read())


def wait_up(port):
    for _ in range(200):
        try:
            get(f"http://127.0.0.1:{port}/healthz")
            return
        except Exception:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} did not come up")


class Servers:
    def __init__(self, mock_latency_ms, env=None):
        self.mock_latency_ms, self.env = mock_latency_ms, env or {}

    def __enter__(self):
        self.cfg = tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False)
        self.cfg.write(CONFIG)
        self.cfg.close()
        self.mock = subprocess.Popen([BIN / "semcache-mock", "--port", str(MOCK_PORT), "--latency-ms",
                                      str(self.mock_latency_ms), "--token-delay-ms", "0", "--threads", "2"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        env = dict(os.environ, SEMCACHE_KEY_LOAD=KEY, **self.env)
        self.proxy = subprocess.Popen([BIN / "semcache", "--config", self.cfg.name], env=env,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_up(MOCK_PORT)
        wait_up(PROXY_PORT)
        return self

    def upstream_calls(self):
        return get(f"http://127.0.0.1:{MOCK_PORT}/mock/stats")["chat_requests"]

    def stats(self):
        return get(f"http://127.0.0.1:{PROXY_PORT}/stats")

    def __exit__(self, *exc):
        for p in (self.proxy, self.mock):
            p.terminate()
            p.wait(timeout=30)
        os.unlink(self.cfg.name)


def loadgen(prompts, mode, concurrency, duration_s=15, extra=()):
    cmd = [BIN / "semcache_loadgen", "--port", str(PROXY_PORT), "--key", KEY, "--prompts", prompts, "--mode", mode,
           "--concurrency", str(concurrency), "--duration-s", str(duration_s), *extra]
    out = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if out.returncode not in (0, 1):
        raise RuntimeError(out.stderr)
    return json.loads(out.stdout)


def prompt_file(name, texts):
    path = EVAL / f"load_{name}.jsonl"
    path.write_text("".join(json.dumps({"text": t}) + "\n" for t in texts))
    return str(path)


def save(name, result):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f"load_{name}.json").write_text(json.dumps(result, indent=2))
    lg = result.get("loadgen", {})
    print(f"[{name}] rps={lg.get('rps', 0):.0f} p50={lg.get('latency', {}).get('p50_ms', 0):.1f}ms "
          f"p99={lg.get('latency', {}).get('p99_ms', 0):.1f}ms upstream={result.get('upstream_calls')} "
          f"results={ {k: v['count'] for k, v in lg.get('by_result', {}).items()} }", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--quick", action="store_true", help="shorter runs, for checking the script")
    args = ap.parse_args()
    only = set(filter(None, args.only.split(",")))
    dur = 5 if args.quick else 15
    qqp = [json.loads(l)["text"] for l in (EVAL / "qqp_texts.jsonl").read_text().splitlines()]
    hot = prompt_file("hot", qqp[:200])            # the cached working set
    fresh = prompt_file("fresh", qqp[200:3200])     # never cached: misses
    big = prompt_file("big", qqp[3200:13200])       # 10K entries for the index scenario
    big_sample = prompt_file("big_sample", qqp[3200:3700])

    def want(n):
        return not only or n in only

    if want("exact_hit") or want("semantic_hit"):
        with Servers(0) as s:
            loadgen(hot, "once", 8)  # populate
            base = s.upstream_calls()
            if want("exact_hit"):
                r = loadgen(hot, "cycle", 32, dur)
                save("exact_hit", {"loadgen": r, "stats": s.stats(), "upstream_calls": s.upstream_calls() - base})
            if want("semantic_hit"):
                base = s.upstream_calls()
                r = loadgen(hot, "case", 32, dur)
                save("semantic_hit", {"loadgen": r, "stats": s.stats(), "upstream_calls": s.upstream_calls() - base})

    if want("miss"):
        with Servers(0) as s:
            r = loadgen(fresh, "once", 32)
            save("miss", {"loadgen": r, "stats": s.stats(), "upstream_calls": s.upstream_calls()})

    if want("mixed"):
        for name, env in (("mixed", {}), ("mixed_nocache", {"SEMCACHE_CACHE__ENABLED": "false"})):
            with Servers(300, env) as s:
                loadgen(hot, "once", 8)
                base = s.upstream_calls()
                r = loadgen(hot, "mix", 64, 2 * dur, ["--new-prompts", fresh, "--mix", "0.5,0.3"])
                save(name, {"loadgen": r, "stats": s.stats(), "upstream_calls": s.upstream_calls() - base,
                            "mock_latency_ms": 300, "mix": "50% exact repeat, 30% case variant, 20% new"})

    if want("herd"):
        for name, env in (("herd_coalescing", {}), ("herd_no_coalescing", {"SEMCACHE_COALESCER__ENABLED": "false"})):
            with Servers(500, env) as s:
                r = loadgen(hot, "herd", 50, extra=["--rounds", "10" if args.quick else "20"])
                save(name, {"loadgen": r, "stats": s.stats(), "upstream_calls": s.upstream_calls(),
                            "mock_latency_ms": 500})

    if want("index"):
        for name, kind in (("index_brute", "brute"), ("index_hnsw", "auto")):
            with Servers(0, {"SEMCACHE_INDEX__KIND": kind}) as s:
                t0 = time.time()
                loadgen(big, "once", 16)
                populate_s = time.time() - t0
                r = loadgen(big_sample, "case", 16, dur)
                save(name, {"loadgen": r, "stats": s.stats(), "upstream_calls": None, "entries": 10000,
                            "populate_s": populate_s, "index_kind": kind})


if __name__ == "__main__":
    main()
