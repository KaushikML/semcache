#!/usr/bin/env bash
# End-to-end check of the container packaging: builds the image, starts docker-compose.yml (demo:
# semcache + mock), and verifies health, dashboard, MISS -> HIT, persistence across a restart
# (named volume, non-root user), prompt redaction in logs, and graceful SIGTERM shutdown.
# Usage: scripts/docker-smoke.sh            (needs docker with the compose plugin, and curl)
#        SEMCACHE_SMOKE_NO_BUILD=1 scripts/docker-smoke.sh   uses an already built semcache:local (CI)
set -euo pipefail
cd "$(dirname "$0")/.."

compose=(docker compose -p semcache-smoke)
base=http://127.0.0.1:8080
key=sk-sc-demo-public
prompt="What is the smoke test refund policy $RANDOM?"

cleanup() { "${compose[@]}" down -v --remove-orphans >/dev/null 2>&1 || true; }
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; "${compose[@]}" logs --no-color --tail 50 >&2 || true; exit 1; }
ok() { echo "  ok: $*"; }

chat() {
  curl -s -o /dev/null -D - -H "Authorization: Bearer $key" -H 'Content-Type: application/json' \
    -d "{\"model\":\"mock-1\",\"messages\":[{\"role\":\"user\",\"content\":\"$prompt\"}]}" \
    "$base/v1/chat/completions" | tr -d '\r' | sed -n 's/^[Xx]-[Ss]emcache: //p'
}

wait_ready() {
  for _ in $(seq 120); do
    if curl -sf "$base/readyz" | grep -q '"ready"'; then return 0; fi
    sleep 0.5
  done
  fail "/readyz never reported ready"
}

build=(--build)
[[ "${SEMCACHE_SMOKE_NO_BUILD:-0}" == 1 ]] && build=(--no-build)
echo "start (${build[*]})"
"${compose[@]}" up -d "${build[@]}" --wait || fail "compose up"
wait_ready
ok "healthy and ready (embedder loaded)"

[[ "$(docker inspect -f '{{.Config.User}}' "$("${compose[@]}" ps -q semcache)")" == semcache ]] || fail "not running as the semcache user"
ok "runs as non-root user"

code=$(curl -s -o /dev/null -w '%{http_code}' "$base/dashboard")
[[ "$code" == 200 ]] || fail "/dashboard returned $code"
curl -s -D - -o /dev/null "$base/dashboard" | grep -qi "content-security-policy: default-src 'self'" || fail "no CSP"
ok "dashboard served with CSP"

[[ "$(chat)" == MISS ]] || fail "first request should be MISS"
[[ "$(chat)" == HIT ]] || fail "second request should be HIT"
ok "MISS then HIT through the mock"

# Persistence: entries reach the WAL within fsync_interval_ms (1 s), then survive a restart.
sleep 2
"${compose[@]}" restart semcache >/dev/null
wait_ready
[[ "$(chat)" == HIT ]] || fail "cache did not survive a restart (volume or permissions)"
ok "cache survives a container restart (named volume)"

if "${compose[@]}" logs --no-color semcache | grep -qF "$prompt"; then fail "prompt text found in logs"; fi
ok "prompt text not in logs"

start=$(date +%s)
"${compose[@]}" stop semcache >/dev/null
elapsed=$(( $(date +%s) - start ))
status=$(docker inspect -f '{{.State.ExitCode}}' "$("${compose[@]}" ps -aq semcache)")
[[ "$status" == 0 ]] || fail "exit code $status on SIGTERM"
ok "SIGTERM: clean exit (code 0) in ${elapsed}s"

echo "docker smoke: all checks passed"
