# ADR-002: Context key composition; hash the last K prior messages

- Status: Accepted
- Date: 2026-10-18

## Context
Only the last user message is embedded. Everything else that influences the answer must match
**exactly**, or a semantically similar question will receive an answer generated under different
instructions, parameters or conversation state.

## Options for prior conversation history
1. Hash **all** prior turns. Safest, but long chats almost never hit.
2. Hash the **last K** prior non-system messages. Higher hit rate in multi-turn apps.
3. Cache single-turn requests only.

## Decision
Option 2, `cache.history_k = 2` by default (the previous user + assistant pair), configurable
per tenant, with `-1` meaning all. **System/developer messages are always included in full**,
regardless of K, as are model, all sampling params (absent encoded as absent, not as the API
default), `max_tokens`, `stop`, `seed`, `response_format`, `tools`, `tool_choice`, and the tenant
id (first field). `stream` is excluded. Fields are length-prefixed and JSON values canonicalized
(sorted keys), then SHA-256 is taken. A version tag `v1` is prefixed so key format changes
invalidate old entries cleanly.

## Tradeoffs / consequences
- **Known correctness risk:** two conversations that diverged more than K messages ago but have
  identical last-K messages and a similar final question can share an answer, even if early
  context (e.g. "I'm on plan B") should change it. This is a deliberate hit-rate vs correctness
  trade, documented in the README limitations. Phase 7 will measure false-hit rate vs K if time
  allows. Tenants with stateful chats should set `history_k = -1`.
- Partition count grows with distinct (system prompt × last-K) combinations. Most are tiny, which
  suits brute force.
- Any parameter we fail to include is a correctness bug, so the canonicalizer has unit tests that
  assert each listed field changes the key.
