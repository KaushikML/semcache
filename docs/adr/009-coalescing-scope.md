# ADR-009: Coalescing on exact flight key; near-duplicate coalescing optional

- Status: Accepted
- Date: 2026-10-18

## Context
Thundering herds (retries, fan-out, page-load bursts) send many identical requests before the
first one finishes, so a cache-on-completion alone does not help them.

## Options
1. Coalesce on the exact flight key = `(context key, prompt hash)`. Decided before embedding.
2. Also coalesce near-duplicates: after embedding, search the in-flight set with the similarity
   threshold.
3. No coalescing.

## Decision
Option 1 by default, with option 2 behind `coalescer.near_duplicate = false`. A flight holds an
append-only chunk log. Followers replay from the start and then tail live, so streaming followers
receive tokens at the leader's pace. Followers get `X-Semcache: COALESCED`. If the leader fails,
every follower gets the same error: no automatic retry, to avoid a retry storm doubling upstream
load. The waiter count is bounded per flight. Only cacheable requests coalesce (BYPASS requests
never do).

## Tradeoffs / consequences
- + Exact-key coalescing has no false-positive risk beyond the cache's own, and is cheap: one
  mutex-guarded map lookup before any CPU work.
- − Near-duplicate coalescing can only be decided after the embed (~5 ms est.), so followers are found later
  and inherits the semantic false-hit risk. That is why it is off by default.
- − A slow leader slows all followers. That is intended, since they wanted the same answer.
- − Client disconnect of the leader: the upstream read continues while followers exist
  (design.md §8).
