# ADR-012: Default similarity threshold chosen by a false-hit budget

- Status: Accepted
- Date: 2026-10-23

## Context
`cache.similarity_threshold` trades hits against wrong answers: a semantic HIT on a question that
only *looks* like a cached one serves the wrong answer, the worst failure a cache can have. The
design used 0.90 as a placeholder until it could be measured (Phase 7).

## Options
1. Keep 0.90 (a common value in semantic-cache tools).
2. Maximize F1 or accuracy on a labelled set.
3. **Pick the lowest threshold that keeps the false-hit rate under a budget** on look-alike
   questions that need a different answer.

## Decision
Option 3 with a 1% budget on QQP's non-duplicate pairs (topically close questions labelled as
different), measured with the product's own embedder: **0.97** (0.69% false hits; 0.96 gives
1.06%). Recall on QQP duplicates at 0.97 is 12.4%. At 0.90 it would be 42.5%, with 6.3% false hits.
Details: [bench/results/phase7](../../bench/results/phase7/README.md).

## Tradeoffs / consequences
- + Wrong answers are rare by construction. F1/accuracy would weigh a wrong answer the same as a
  missed saving, which is the wrong cost model for a cache.
- − With all-MiniLM-L6-v2 the semantic layer then catches near-rewordings only (paraphrase median
  cosine 0.655 on the FAQ set). Most savings come from exact repeats and coalescing.
- − Some errors are below any usable threshold: negation ("warranty cover" vs "warranty *not*
  cover" = 0.974). Mitigations: per-deployment threshold, `X-Semcache-Bypass`, tags + purge, or a
  stronger model / re-ranker (future work).
- QQP is one domain. Deployments should re-run `tools/analyze_threshold.py` on their own pairs.
