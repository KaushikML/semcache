# ADR-013: The exact key folds case, spacing and sentence-final punctuation

- Status: Accepted
- Date: 2026-10-24

## Context
The exact fast path (design F4) hashed the final user message after NFC and trimming only. Trivial
variants such as "tell me the privacy policy" vs "Tell me the privacy policy." therefore took the
embedding path. Measured with the product's embedder, punctuation alone moves the score (that pair:
0.9788), so near the 0.97 threshold some trivial variants could even miss. A user reported short
queries missing; the main cause is different words ("tell me the privacy policy" vs "tell privacy
policy" = 0.830), which no exact-key rule can fix, but trivial variants should never depend on the
model.

## Options
1. Keep the exact key as is. Simple; trivial variants pay an embedding and depend on the threshold.
2. Fold case, whitespace and sentence-final punctuation only.
3. Also drop all punctuation and stopwords. Rejected: "1.2" vs "12", "C++" vs "C", "not" and
   "what" vs "where" would merge into wrong answers.
4. Lower the threshold. Rejected: it does not reach 0.830 at any acceptable false-hit rate
   (QQP: 16% false hits at 0.83, ADR-012).

## Decision
Option 2. The exact key is SHA-256 of: NFC + Unicode case folding (utf8proc), whitespace runs
collapsed to one space and trimmed, and a trailing run of `.` `?` `!` (also `…` `。` `？` `！`)
removed when it follows a letter (optionally one space between). Everything else stays
significant: words, internal punctuation, a final mark after a digit ("5!"), accents. The model's
input is unchanged.

## Tradeoffs / consequences
- + Case, spacing and final-punctuation variants are exact HITs: no embedding, no threshold.
- + No new false-hit risk from case: the model is uncased, so case variants already embed
  identically.
- − Different words still go through the model; this does not raise semantic recall.
- − Entries persisted before this change keep their old exact keys. Identical repeats of them
  still HIT through the semantic path (similarity 1.0). Exact-only entries (prompts over 256
  tokens, never embedded) become unreachable until they expire, unless their text was already
  lower-case with no final punctuation. Accepted: a cache miss costs one upstream call.
- The Phase 7 "semantic HIT" load scenario used random-case variants, which are now exact HITs; the
  load generator now uses accent variants (identical embedding, different exact key). The
  published Phase 7 numbers were measured before this change.
