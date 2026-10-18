# ADR-003: vcpkg manifest mode + prebuilt, pinned ONNX Runtime

- Status: Accepted
- Date: 2026-10-18

## Context
Dependencies: Boost (Asio, Beast), OpenSSL, nlohmann-json, spdlog, toml++, utf8proc, GoogleTest,
Google Benchmark, ONNX Runtime. Neither vcpkg nor Conan is installed on the dev box. CI must be
reproducible and reasonably fast, including sanitizer builds.

## Options
1. vcpkg for everything, including the `onnxruntime` port.
2. vcpkg for everything except ORT, which comes from the official prebuilt release tarball.
3. Conan 2 for everything.

## Decision
Option 2. `vcpkg.json` with a pinned `builtin-baseline`, vcpkg added as a git submodule (or
bootstrapped by a script) so the toolchain version is pinned too. ORT is fetched by CMake
(`FetchContent`/`file(DOWNLOAD)`) from the GitHub release for a pinned version with a pinned
SHA-256, and exposed as an imported target `onnxruntime::onnxruntime`. CI caches the vcpkg binary
cache and the ORT tarball.

## Tradeoffs / consequences
- + No 30+ minute ORT source build. ORT release binaries are what most production users run.
- − ORT is not built with sanitizers. ASan/TSan will not instrument ORT internals, and TSan may
  report false positives from inside its thread pool, so we set `intra_op_num_threads = 1`,
  disable ORT's global thread pool, and keep a narrow suppressions file. The TSan job can also run
  with a fake Embedder to keep our own code fully instrumented.
- − Linux x64 only out of the box. Other platforms need a different tarball and hash.
- vcpkg is widely recognised in C++ shops, and manifest mode keeps dependencies declarative.
