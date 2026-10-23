#!/usr/bin/env python3
"""Prepares the Phase 7 evaluation inputs.

  data/eval/qqp_texts.jsonl   unique questions from a seeded sample of QQP validation pairs
  data/eval/qqp_pairs.json    {"pairs": [[i, j, label], ...]} indexing into qqp_texts
  bench/data/faq_eval.jsonl   the synthetic FAQ set (committed; tools/faq_eval_data.py)
  data/eval/faq_texts.jsonl   its texts, in the same order

QQP (Quora Question Pairs, GLUE) is downloaded at a pinned revision into the git-ignored data/
directory and never committed. Embeddings are computed by the product's C++ embedder
(build/release/semcache_embed_texts), not here.

Usage: .venv/bin/python tools/prepare_eval.py [--pairs 10000] [--seed 7]
"""

import argparse
import json
import pathlib
import random
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import faq_eval_data  # noqa: E402

QQP_REPO = "nyu-mll/glue"
QQP_REVISION = "bcdcba79d07bc864c1c254ccfcedcce55bcc9a8c"


def write_jsonl(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    from datasets import load_dataset

    ds = load_dataset(QQP_REPO, "qqp", split="validation", revision=QQP_REVISION)
    idx = list(range(len(ds)))
    random.Random(args.seed).shuffle(idx)
    idx = idx[: args.pairs]

    texts, index, pairs = [], {}, []

    def tid(t):
        t = t.strip()
        if t not in index:
            index[t] = len(texts)
            texts.append(t)
        return index[t]

    for i in idx:
        row = ds[i]
        pairs.append([tid(row["question1"]), tid(row["question2"]), int(row["label"])])
    out = ROOT / "data/eval"
    write_jsonl(out / "qqp_texts.jsonl", ({"text": t} for t in texts))
    (out / "qqp_pairs.json").write_text(json.dumps({
        "source": f"{QQP_REPO}@{QQP_REVISION} qqp/validation", "seed": args.seed, "pairs": pairs}))
    dup = sum(p[2] for p in pairs)
    print(f"QQP: {len(pairs)} pairs ({dup} duplicates, {len(pairs) - dup} not), {len(texts)} unique texts")

    faq = faq_eval_data.rows()
    write_jsonl(ROOT / "bench/data/faq_eval.jsonl", faq)
    write_jsonl(out / "faq_texts.jsonl", ({"text": r["text"]} for r in faq))
    kinds = {k: sum(r["kind"] == k for r in faq) for k in ("canonical", "paraphrase", "negative")}
    print(f"FAQ: {len(faq)} questions {kinds}")


if __name__ == "__main__":
    main()
