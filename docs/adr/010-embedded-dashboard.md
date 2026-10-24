# ADR-010: Dashboard as static files embedded in the binary

- Status: Accepted (implemented in Phase 8)
- Date: 2026-10-18 (implementation notes 2026-10-24)

## Context
The dashboard has to explain the project to a non-technical viewer in 10 seconds, work offline
and add no build tooling.

## Options
1. Separate SPA (React/Vite) with its own server or CDN. Rejected: requires Node and a build step.
2. Static files served from a directory on disk next to the binary.
3. **Static HTML/CSS/vanilla JS compiled into the binary** (CMake generates a `.cpp` with byte
   arrays), served at `/dashboard`, with an optional override directory for live editing
   during development.

## Decision
Option 3. Data comes from the same-origin `/stats`, `/stats/stream` (SSE) and
`/v1/chat/completions`. No external fonts, scripts or CDNs. Strict CSP header:
`default-src 'self'`.

## Tradeoffs / consequences
- + Single artefact. `docker compose up` gives a working demo with no extra container.
- + Same origin means no CORS, and the CSP blocks third-party loads.
- − Every UI change requires a rebuild, mitigated by the dev override.
- − Vanilla JS for charts (SVG by hand) is more code than a library. That is acceptable at this
  size.

## Implementation notes (Phase 8)
- `cmake/embed_web.cmake` runs in CMake script mode at build time and turns every `web/*` file into
  a `kData<i>` array plus a name table in `build/<preset>/generated/web_assets.cpp`;
  `web::embedded_file(name)` looks names up there. No `xxd`, no Python in the build.
- The override is the config key `server.dashboard_dir` (TOML/env), not a CLI flag: every other
  setting lives in config. When it is set, files come only from that directory.
- Only bare file names are served (`/dashboard/<name>`, no `/`, `\`, or leading `.`), so a
  request can never leave the directory. Anything else is 404.
- Headers on every dashboard response: `Content-Security-Policy: default-src 'self'; script-src
  'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; base-uri 'none';
  form-action 'none'; frame-ancestors 'none'`, `X-Content-Type-Options: nosniff`,
  `Referrer-Policy: no-referrer`, `Cache-Control: no-cache`. The page therefore has no inline
  script or style, and builds DOM with `textContent` only (no `innerHTML`).
- Tested at two levels: C++ integration tests (headers, content types, path refusal, override,
  `/stats` and `/stats/stream`) and `tools/dashboard_e2e.py`, which drives the real binaries in
  headless Chromium with the CSP enforced and fails on any console error.
