# ADR-010: Dashboard as static files embedded in the binary

- Status: Proposed (implemented in Phase 8)
- Date: 2026-10-18

## Context
The dashboard has to explain the project to a non-technical viewer in 10 seconds, work offline
and add no build tooling.

## Options
1. Separate SPA (React/Vite) with its own server or CDN. Rejected: requires Node and a build step.
2. Static files served from a directory on disk next to the binary.
3. **Static HTML/CSS/vanilla JS compiled into the binary** (CMake generates a `.cpp` with byte
   arrays), served at `/dashboard`, with an optional `--dashboard-dir` override for live editing
   during development.

## Decision
Option 3. Data comes from the same-origin `/stats`, `/stats/stream` (SSE) and
`/v1/chat/completions`. No external fonts, scripts or CDNs. Strict CSP header:
`default-src 'self'`.

## Tradeoffs / consequences
- + Single artefact. `docker compose up` gives a working demo with no extra container.
- + Same origin means no CORS, and the CSP blocks third-party loads.
- − Every UI change requires a rebuild, mitigated by the dev override flag.
- − Vanilla JS for charts (SVG by hand) is more code than a library. That is acceptable at this
  size.
