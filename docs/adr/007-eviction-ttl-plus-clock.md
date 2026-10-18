# ADR-007: Eviction — TTL + CLOCK (approximate LRU) under a byte budget

- Status: Accepted
- Date: 2026-10-18

## Context
The cache needs freshness (answers go stale) and a hard memory cap. Response sizes vary by about
10×, so an entry-count limit does not bound memory. Hits run under a **shared** partition lock,
and exact LRU would need a list splice, which is a write, on every hit.

## Options
1. TTL only. No memory bound.
2. Exact LRU (intrusive list) by entry count. Needs a write lock on every hit.
3. Exact LRU by bytes, with a separate LRU mutex. Every hit contends on one global mutex.
4. **TTL + CLOCK (second chance) by bytes.** A hit sets an atomic `referenced` bit. The writer
   sweeps a clock hand when over budget.
5. Sampled LRU (Redis-style): an atomic last-access timestamp, evicting the oldest of S random
   samples.

## Decision
Option 4. A global byte budget (`cache.max_bytes`) plus a per-tenant quota (`tenants[].max_bytes`)
stops one tenant evicting everyone else. TTL is per entry (`default_ttl_s`, overridable per
tenant). Expiry is lazy (lookups skip expired entries) plus an active sweep by the maintenance
thread, which enqueues deletes to the writer. Eviction happens on the writer thread before an
insert that would exceed either budget.

## Tradeoffs / consequences
- + Hits never take an exclusive lock or a shared mutex. They do one relaxed atomic store.
- + Bytes are what we promise to bound.
- − CLOCK approximates LRU. It is slightly worse under some scan patterns, which barely matters
  for a cache whose hit decisions are semantic anyway.
- − The per-tenant quota means a lone tenant can't use the whole budget unless its quota allows
  it. Quotas are configurable and default to the global budget.
