# ADR-011: Use the ONNX export published with the model, pinned by revision and SHA-256

- Status: Accepted
- Date: 2026-10-19

## Context
The product needs all-MiniLM-L6-v2 as an ONNX graph plus its `vocab.txt`. The original plan was a
Python export script (torch → ONNX). That puts torch (~200 MB) on every developer machine and in
CI, and adds an export step whose output depends on the torch/exporter version. The model repo on
HuggingFace already publishes `onnx/model.onnx` (fp32), and also O1–O4 optimized and int8 variants.

## Options
1. Export ourselves with `torch.onnx.export` in a tooling script.
2. Download the published `onnx/model.onnx` at a pinned repo revision, verified by SHA-256.
3. Download a quantized variant (`model_quint8_avx2.onnx`) for speed.

## Decision
Option 2. `scripts/fetch-model.sh` downloads `onnx/model.onnx` and `vocab.txt` from
`sentence-transformers/all-MiniLM-L6-v2@1110a243…`, checks both SHA-256s, and fails on any
mismatch. `tools/make_golden.py` checks that this exact file + HF token ids + mean pooling
reproduces sentence-transformers (measured: min cosine 0.9999999 over 125 sentences). So any C++
drift can only come from our tokenizer or pooling.

## Tradeoffs / consequences
- + No torch in CI or for users; the model is a fixed, hash-verified input, like the ORT tarball (ADR-003).
- + Graph inputs/outputs (`input_ids`, `attention_mask`, `token_type_ids` → `last_hidden_state`) are
  checked when the model loads, so a different export fails at startup, not silently.
- − Depends on HuggingFace hosting that revision. Mitigation: CI caches the files, and the SHA pins
  mean a re-hosted copy can be used without trusting it.
- Option 3 measured on the dev box: 2–3× faster (8.0 vs 22.2 ms at 32 tokens) but min cosine
  0.9725 vs sentence-transformers, which fails the Phase 2 gate (> 0.999) and shifts scores near
  the hit threshold. Revisit in Phase 7, when the threshold curve can show what int8 costs in false hits.
