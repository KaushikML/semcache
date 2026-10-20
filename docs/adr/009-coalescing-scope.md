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

## Implementation notes (Phase 4, 2026-10-20)
- **Flight key = context key + prompt hash + `stream` + `stream_options.include_usage`.** Followers
  replay the leader's bytes verbatim, so requests that need a different response *format* get
  separate flights. They still share cache entries afterwards.
- **What is shared is what the leader's client receives.** The leader serves through a
  `LeaderWriter` that records every write (head, body or SSE chunks, end) into the flight. So
  followers get the same HIT, MISS, gateway error (502/504) or in-band stream error as the leader,
  with their own `X-Semcache: COALESCED` and latency headers. All other headers are shared.
- **Leader vanishes before any response head** (cancelled, exception): followers serve
  themselves independently (fail open), counted in `semcache_coalesce_independent_total`.
  **Mid-stream:** followers get the chunks so far plus an SSE error event, then the connection closes.
- **Leader's client disconnects:** the upstream read continues while followers exist (or while
  the response is being captured for the cache), bounded by `upstream.total_timeout_ms`.
- **Flight lifetime:** a flight stays joinable until the writer thread has applied the leader's
  insert. That closes the asynchronous-insert window: a duplicate arriving after the response
  finished but before the entry is visible replays the flight instead of calling upstream again.
- **Bounded:** at most `coalescer.max_waiters_per_flight` followers; a streamed flight log is
  capped at `upstream.max_response_bytes`, after which followers get an error event.
- **Near-duplicate coalescing (option 2) is not implemented.** No config flag exists for it.
  Revisit only if Phase 7 shows herds of paraphrases rather than of identical requests.
- Mutation-tested: making every request a leader fails 8 of the 12 herd tests; letting the
  leader stop when its own client leaves fails the unit test and
  `Herd.FollowersAloneKeepTheSharedCallAlive`.
