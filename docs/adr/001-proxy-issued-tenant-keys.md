# ADR-001: Proxy-issued tenant keys; upstream key held server-side

- Status: Accepted
- Date: 2026-10-18

## Context
Every cache entry must belong to exactly one tenant, and one tenant must never read another's
answers. We also need a demo mode where a public browser page can call the proxy without any real
secret being reachable.

## Options
1. **Proxy-issued keys.** Clients send `sk-sc-…`. The proxy maps it to a tenant id and calls
   upstream with its own key from `SEMCACHE_UPSTREAM_API_KEY`.
2. **Pass-through.** Clients send their OpenAI key and tenant = SHA-256(key).
3. Both.

## Decision
Option 1. Tenants are declared in config with `key_env`, the name of the env var that holds the
key. Only SHA-256(key) is kept in memory, and lookup goes through a hash map keyed by that digest.

## Tradeoffs / consequences
- + The upstream key never leaves the server. The demo tenant cannot leak anything real.
- + The tenant id is stable when keys rotate (change the env var, keep the cache).
- + Per-tenant quotas, TTLs and thresholds have a natural home.
- − All tenants share one upstream account (billing is not split upstream). That is acceptable
  for an internal gateway, and per-tenant upstream keys can be added later as `upstream_key_env`
  per tenant.
- − Adding a tenant requires a config change and a restart (no admin API for keys in v1).
