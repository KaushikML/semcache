#!/usr/bin/env python3
"""Phase 9: memory and startup of the installed tree, for the sizing advice in docs/deploy.md.

Runs the *installed* binaries (cmake --install --strip, the same layout as the Docker image)
against the mock upstream in live mode with persistence on, and records resident memory (VmRSS,
VmHWM from /proc) and timings at three points:
  1. idle, after the model is loaded (/readyz ready),
  2. after N unique prompts were answered and stored (one context, so the partition becomes HNSW),
  3. after a restart, once recovery from snapshot/WAL finished.
Mock answers are --response-words long (default 300 words, ~2 KB, the design's assumed size).

    cmake --install build/release --prefix /tmp/semcache-install --strip
    .venv/bin/python tools/run_memory_eval.py --prefix /tmp/semcache-install --entries 10000
Writes bench/results/phase9/memory.json and memory.md.
"""

import argparse
import json
import os
import pathlib
import platform
import random
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "bench/results/phase9"
KEY = "sk-sc-memeval"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read())


def wait(cond, timeout_s, what):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            if cond():
                return
        except Exception:
            pass
        time.sleep(0.1)
    raise RuntimeError(f"timeout waiting for {what}")


def mem_mib(pid):
    fields = {}
    for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
        k, _, v = line.partition(":")
        if k in ("VmRSS", "VmHWM"):
            fields[k] = round(int(v.split()[0]) / 1024, 1)
    return fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", required=True, help="install prefix (bin/, lib/)")
    ap.add_argument("--entries", type=int, default=10000)
    ap.add_argument("--response-words", type=int, default=300)
    ap.add_argument("--loadgen", default=str(ROOT / "build/release/semcache_loadgen"))
    args = ap.parse_args()
    prefix = pathlib.Path(args.prefix).resolve()
    work = pathlib.Path(tempfile.mkdtemp(prefix="semcache-mem-"))
    proxy_port, mock_port = free_port(), free_port()

    # Diverse, deterministic prompts: 12 random whole words from the model's vocabulary. Templated
    # prompts are near-duplicates to the model and would be served as semantic HITs, not stored.
    words = [w for w in (ROOT / "models/vocab.txt").read_text().split() if w.isalpha() and len(w) > 2]
    rng = random.Random(42)
    prompts = work / "prompts.jsonl"
    with prompts.open("w") as f:
        for _ in range(args.entries):
            f.write(json.dumps({"text": " ".join(rng.choices(words, k=12)) + "?"}) + "\n")
    cfg = work / "semcache.toml"
    cfg.write_text(f"""
mode = "live"
[server]
host = "127.0.0.1"
port = {proxy_port}
[upstream]
base_url = "http://127.0.0.1:{mock_port}"
[embedder]
model_path = "{ROOT}/models/all-MiniLM-L6-v2.onnx"
vocab_path = "{ROOT}/models/vocab.txt"
[persistence]
dir = "{work}/data"
[limits]
rps = 100000
burst = 100000
[logging]
level = "warn"
[[tenants]]
id = "mem"
key_env = "SEMCACHE_KEY_MEM"
""")
    env = {k: v for k, v in os.environ.items() if k != "SEMCACHE_UPSTREAM_API_KEY"}
    env["SEMCACHE_KEY_MEM"] = KEY
    base = f"http://127.0.0.1:{proxy_port}"
    mock = subprocess.Popen([prefix / "bin/semcache-mock", "--port", str(mock_port), "--latency-ms", "0",
                             "--token-delay-ms", "0", "--response-words", str(args.response_words)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def start():
        t0 = time.time()
        p = subprocess.Popen([prefix / "bin/semcache", "--config", cfg], env=env, cwd=work,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait(lambda: get(base + "/readyz")["status"] == "ready", 300, "ready")
        return p, time.time() - t0

    def stop(p):
        p.terminate()
        p.wait(timeout=60)

    result = {"entries_requested": args.entries, "response_words": args.response_words,
              "machine": {"cpu": next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), ""),
                          "nproc": os.cpu_count(), "kernel": platform.release()}}
    try:
        wait(lambda: get(f"http://127.0.0.1:{mock_port}/healthz") is not None, 30, "mock")
        proxy, ready_s = start()
        result["idle"] = {"ready_s": round(ready_s, 2), **mem_mib(proxy.pid)}
        print("idle:", result["idle"])

        t0 = time.time()
        out = subprocess.run([args.loadgen, "--port", str(proxy_port), "--key", KEY, "--prompts", str(prompts),
                              "--mode", "once", "--concurrency", "8", "--duration-s", "3600"],
                             capture_output=True, text=True, check=False)
        if out.returncode not in (0, 1):
            raise RuntimeError(out.stderr)
        # Inserts are applied asynchronously by the writer; wait until they have all landed.
        try:
            wait(lambda: get(base + "/stats")["cache"]["entries"] >= args.entries * 0.99, 600, "inserts")
        except RuntimeError:
            s = get(base + "/stats")
            print("requests:", s["requests"], "entries:", s["cache"]["entries"])
            print("\n".join(l for l in urllib.request.urlopen(base + "/metrics").read().decode().splitlines()
                            if l.startswith(("semcache_store_", "semcache_bypass_total"))))
            raise
        stats = get(base + "/stats")["cache"]
        result["filled"] = {"fill_s": round(time.time() - t0, 1), "entries": stats["entries"],
                            "cache_bytes_mib": round(stats["bytes"] / 2**20, 1), **mem_mib(proxy.pid)}
        print("filled:", result["filled"])
        time.sleep(2)  # past fsync_interval_ms
        stop(proxy)
        disk = sum(f.stat().st_size for f in (work / "data").rglob("*") if f.is_file())

        proxy, ready_s = start()
        stats = get(base + "/stats")["cache"]
        result["restarted"] = {"ready_s": round(ready_s, 2), "entries": stats["entries"],
                               "disk_mib": round(disk / 2**20, 1), **mem_mib(proxy.pid)}
        print("restarted:", result["restarted"])
        stop(proxy)
    finally:
        mock.terminate()
        mock.wait(timeout=30)

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "memory.json").write_text(json.dumps(result, indent=2) + "\n")
    i, f, r = result["idle"], result["filled"], result["restarted"]
    (OUT / "memory.md").write_text(f"""# Memory and startup (Phase 9)

Generated by `tools/run_memory_eval.py` on {result['machine']['cpu']} ({result['machine']['nproc']} vCPUs,
kernel {result['machine']['kernel']}). Installed tree (`cmake --install --strip`, the image layout),
live mode against the mock, persistence on, mock answers of {args.response_words} words.
{args.entries:,} distinct prompts (12 random vocabulary words each, seed 42) in one context
(one partition, HNSW after 5,000 entries).

| point | ready after | entries | cache bytes (MiB) | RSS (MiB) | peak RSS (MiB) |
|---|---:|---:|---:|---:|---:|
| idle, model loaded | {i['ready_s']} s | 0 | 0 | {i['VmRSS']} | {i['VmHWM']} |
| after {args.entries:,} misses stored | – | {f['entries']:,} | {f['cache_bytes_mib']} | {f['VmRSS']} | {f['VmHWM']} |
| restarted, recovered | {r['ready_s']} s | {r['entries']:,} | – | {r['VmRSS']} | {r['VmHWM']} |

On disk after the run: {r['disk_mib']} MiB (snapshot + WAL). Filling took {f['fill_s']} s.
""")
    print("wrote", OUT / "memory.md")


if __name__ == "__main__":
    main()
