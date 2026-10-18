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
