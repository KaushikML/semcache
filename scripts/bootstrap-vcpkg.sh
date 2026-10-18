#!/usr/bin/env bash
# Clones vcpkg at the commit pinned in vcpkg.json's builtin-baseline and bootstraps it.
# Usage: scripts/bootstrap-vcpkg.sh [dest]   (default: external/vcpkg)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${1:-$root/external/vcpkg}"
baseline="$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' "$root/vcpkg.json")"
if [[ ! -d "$dest/.git" ]]; then
  git clone --filter=blob:none https://github.com/microsoft/vcpkg "$dest"
fi
git -C "$dest" fetch --quiet origin "$baseline" || true
git -C "$dest" checkout --quiet "$baseline"
if [[ ! -x "$dest/vcpkg" ]]; then
  "$dest/bootstrap-vcpkg.sh" -disableMetrics
fi
echo "vcpkg ready at $dest (baseline $baseline)"
