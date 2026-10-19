# ADR-004: Own C++ WordPiece tokenizer, verified by golden files

- Status: Accepted
- Date: 2026-10-18

## Context
all-MiniLM-L6-v2 uses BERT uncased WordPiece (vocab 30,522, `[CLS] … [SEP]`, max 256 tokens in
sentence-transformers). An ONNX export contains only the transformer. Tokenization must happen in
C++ and must match HuggingFace exactly, or embeddings silently drift. **This is the top technical
risk of Phase 2.**

## Options
1. Own implementation: BasicTokenizer (clean control chars, CJK spacing, whitespace split,
   lowercase, NFD + strip accents, punctuation split) + greedy longest-match WordPiece with `##`.
2. `tokenizers-cpp` (HF Rust tokenizers via FFI): exact parity, but needs a Rust toolchain.
3. `onnxruntime-extensions`: tokenizer inside the graph.

## Decision
Option 1, using utf8proc for NFD decomposition, case folding and Unicode category lookups.
Python tooling generates a golden file: several thousand strings (QQP sample, emoji, CJK, accents,
punctuation runs, control chars, very long words > 100 chars → `[UNK]`, empty and whitespace-only
input) with the HF `BertTokenizerFast` ids. The unit test requires **exact id equality** on every
case. The end-to-end check is cosine > 0.999 vs sentence-transformers (Phase 2 gate).

## Tradeoffs / consequences
- + No Rust in the build. Small, readable, benchmarkable code (a good portfolio artefact).
- − Unicode edge cases are where implementations diverge (Python's `unicodedata` version vs
  utf8proc's Unicode version). Mitigation: golden tests, plus documenting any accepted
  divergences.
- − Only BERT-style WordPiece. Changing to a model with a different tokenizer means new code.
  That is acceptable, since `Embedder` hides it.

## Implementation notes (Phase 2, 2026-10-19)
- **Unicode categories could not be used.** HF's Rust tokenizer uses Unicode tables older than
  utf8proc's (and Python's 15.0): it keeps Cn and most Mc, strips U+1734, and leaves Unicode 16
  scripts unmapped. A category-based implementation mismatched on 803 of 3,125 golden strings.
- **Character classes are generated from HF itself.** `tools/gen_bert_tables.py` runs all
  1,114,112 code points through HF's normalizer and pre-tokenizer and emits range tables
  (`src/embed/bert_tables.inc`, ~1,000 ranges): removed, whitespace, CJK-padded, mapped,
  punctuation, split. utf8proc supplies NFD and lowercase mappings, **only** for the code points
  HF maps, so behaviour is pinned to HF's Unicode version, not to whichever utf8proc vcpkg ships.
- **Exhaustive check:** `tests/data/normalizer_blocks.txt` has the SHA-256 of HF's output per
  4,096-code-point block; the C++ test recomputes all 272 blocks. On a mismatch it dumps the block,
  and `tools/gen_bert_tables.py --diff <dump>` lists the differing code points.
- **Golden strings are synthetic, not QQP:** 70 hand-written edge cases, 55 natural prompts, and
  3,000 seeded random strings sampled across 50 Unicode blocks. This avoids committing Quora text
  and covers more of the normalizer. QQP is used in Phase 7 (downloaded, not committed).
- **Known, accepted divergence:** canonical reordering uses utf8proc's combining classes. A run of
  several *retained* (not stripped) non-starter marks from scripts newer than HF's tables could be
  ordered differently. No golden string hits this, and the per-code-point check cannot see it.
