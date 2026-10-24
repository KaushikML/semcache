#!/usr/bin/env python3
"""Browser end-to-end test of the dashboard (Phase 8).

Starts the real `semcache` (demo mode, real embedder) and `semcache-mock`, opens /dashboard in
headless Chromium (Playwright), runs every scenario card, the try-it box and the reset button, and
checks the page against what the proxy reported. Fails on any console error or CSP violation.
Optionally saves a screenshot (--screenshot) for the docs.

    .venv/bin/pip install -r tools/requirements.txt && .venv/bin/python -m playwright install chromium
    .venv/bin/python tools/dashboard_e2e.py [--bin build/release] [--screenshot docs/img/dashboard.png]

Without root, missing Chromium system libraries can be unpacked locally (see docs/demo-script.md)
and passed via LD_LIBRARY_PATH.
"""

import argparse
import json
import os
import re
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

from playwright.sync_api import expect, sync_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read())


def wait_ready(port, need_embedder, timeout_s=60):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            if not need_embedder:
                get(f"http://127.0.0.1:{port}/healthz")
                return
            if get(f"http://127.0.0.1:{port}/readyz")["status"] == "ready":
                return
        except Exception:
            pass
        time.sleep(0.1)
    raise RuntimeError(f"port {port} not ready")


class Failed(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Failed(what)
    print("  ok:", what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/debug")
    ap.add_argument("--screenshot")
    args = ap.parse_args()
    bin_dir = ROOT / args.bin
    proxy_port, mock_port = free_port(), free_port()
    cfg = tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False)
    # Demo mode as shipped, except a larger per-address burst: this test sends ~30 requests at once.
    cfg.write(f"""
mode = "demo"
[server]
host = "127.0.0.1"
port = {proxy_port}
[upstream]
base_url = "http://127.0.0.1:{mock_port}"
[persistence]
enabled = false
[stats]
stream_interval_ms = 250
[demo]
allowed_upstreams = ["127.0.0.1:{mock_port}"]
per_ip_burst = 40
[logging]
level = "warn"
""")
    cfg.close()
    env = {k: v for k, v in os.environ.items() if k != "SEMCACHE_UPSTREAM_API_KEY"}
    mock = subprocess.Popen([bin_dir / "semcache-mock", "--port", str(mock_port), "--latency-ms", "400",
                             "--token-delay-ms", "5"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proxy = subprocess.Popen([bin_dir / "semcache", "--config", cfg.name], cwd=ROOT, env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    problems = []
    try:
        wait_ready(mock_port, False)
        wait_ready(proxy_port, True)
        base = f"http://127.0.0.1:{proxy_port}"
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 1280, "height": 1400})
            page.on("console", lambda m: m.type in ("error", "warning") and problems.append(f"console {m.type}: {m.text}"))
            page.on("pageerror", lambda e: problems.append(f"page error: {e}"))
            page.goto(base + "/dashboard")

            print("live stats")
            # Waits use locator assertions: they run outside the page, so its CSP (no 'unsafe-eval') stays on.
            expect(page.locator("#pill-mode")).to_have_text(re.compile(r"^demo"))
            check(page.inner_text("#t-requests") == "0", "tiles filled from /stats (0 requests)")
            check("ready" in page.inner_text("#pill-embedder"), "embedder pill says ready")
            check(page.is_visible("#reset") and not page.is_visible("#key-row"), "demo mode: reset shown, no key box")

            def run(name):
                card = page.locator(f'.scenario[data-scenario="{name}"]')
                card.locator("button").click()
                expect(card.locator(".verdict")).to_have_class(re.compile(r"verdict (good|bad)"), timeout=30000)
                badges = card.locator(".steps .badge").all_inner_texts()
                verdict = card.locator(".verdict")
                return badges, verdict.inner_text(), verdict.get_attribute("class"), card.locator(".steps").inner_text()

            print("scenario: repeat")
            badges, text, cls, _ = run("repeat")
            check(badges == ["MISS", "HIT"], f"repeat is MISS then HIT ({badges})")
            check("good" in cls, f"verdict: {text}")

            print("scenario: reword")
            badges, text, cls, steps = run("reword")
            check(badges == ["MISS", "HIT"], f"reworded question is a semantic HIT ({badges})")
            check("similarity" in steps, "similarity shown")

            print("scenario: trap")
            badges, text, cls, steps = run("trap")
            # With all-MiniLM-L6-v2 at 0.97 this is a (known) wrong HIT at 0.974; the page must say so.
            check(badges[0] == "MISS", "trap: first question is a MISS")
            if badges[1] == "HIT":
                check("bad" in cls and "Wrong answer" in text, f"false hit is labelled as wrong: {text}")
            else:
                check("good" in cls, f"not matched: {text}")

            print("scenario: rush")
            calls_before = get(f"http://127.0.0.1:{mock_port}/mock/stats")["chat_requests"]
            badges, text, cls, steps = run("herd")
            calls = get(f"http://127.0.0.1:{mock_port}/mock/stats")["chat_requests"] - calls_before
            check(calls == 1, f"rush made exactly 1 upstream call ({steps.replace(chr(10), ' ')})")
            check("good" in cls, f"verdict: {text}")

            print("try it (streaming)")
            page.fill("#try-prompt", "Do you ship to Canada?")
            page.click("#try button[type=submit]")
            expect(page.locator("#try-badge")).to_have_text("MISS", timeout=30000)
            first = page.inner_text("#try-text")
            check(len(first) > 20, "streamed answer assembled")
            page.click("#try button[type=submit]")
            expect(page.locator("#try-badge")).to_have_text("HIT", timeout=30000)
            check(page.inner_text("#try-text") == first, "repeat served the same answer from the cache")
            check(page.locator("#history li").count() == 2, "history kept in the page")

            print("server view")
            stats = get(base + "/stats")
            expect(page.locator("#t-requests")).to_have_text(str(stats["requests_total"]), timeout=5000)
            check(True, f"request tile matches /stats ({stats['requests_total']})")
            rows = page.locator("#recent tr").count()
            check(rows == min(12, stats["requests_total"]), f"recent table has {rows} rows")
            check("Do you ship" not in json.dumps(stats), "/stats carries no prompt text")
            check(page.get_attribute("#spark-line", "points").count(",") >= 2, "sparkline drawn")

            if args.screenshot:
                page.screenshot(path=str(ROOT / args.screenshot), full_page=True)
                print("  saved", args.screenshot)

            print("reset")
            entries = stats["cache"]["entries"]
            page.click("#reset")
            expect(page.locator("#status-line")).to_have_text(re.compile(r"^cleared"))
            check(page.inner_text("#status-line") == f"cleared {entries} cached answers", page.inner_text("#status-line"))
            check(get(base + "/stats")["cache"]["entries"] == 0, "cache empty after reset")
            browser.close()
        check(not problems, "no console errors or CSP violations" + ("" if not problems else f": {problems}"))
    except Failed as e:
        print("FAILED:", e)
        for pr in problems:
            print("  ", pr)
        return 1
    finally:
        for proc in (proxy, mock):
            proc.terminate()
            proc.wait(timeout=30)
        os.unlink(cfg.name)
    print("dashboard e2e: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
