#!/usr/bin/env bash
# Runs the embedding benchmarks on the Release build and stores raw JSON + machine info.
# Usage: bench/run_embed_bench.sh [build dir]   (default: build/release)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build/release}"
out="$root/bench/results/phase2"
mkdir -p "$out"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"

{
  echo "date: $stamp"
  echo "commit: $(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet || echo '-dirty')"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs)"
  echo "cores: $(nproc)"
  echo "kernel: $(uname -r)"
  echo "compiler: $(grep CMAKE_CXX_COMPILER: "$build/CMakeCache.txt" | cut -d= -f2) ($(grep CMAKE_BUILD_TYPE: "$build/CMakeCache.txt" | cut -d= -f2))"
  echo "onnxruntime: $(grep -m1 'SEMCACHE_ORT_VERSION ' "$root/cmake/onnxruntime.cmake" | grep -o '[0-9.]*')"
} > "$out/embed_$stamp.env.txt"

"$build/semcache_bench_embed" --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
  --benchmark_out="$out/embed_$stamp.json" --benchmark_out_format=json 
echo "results: $out/embed_$stamp.json"
