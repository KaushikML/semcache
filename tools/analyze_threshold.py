#!/usr/bin/env python3
"""Hit vs false-hit curves for cache.similarity_threshold (Phase 7).

Inputs: embeddings from the product's C++ embedder (build/release/semcache_embed_texts) for
  data/eval/qqp_texts.jsonl -> data/eval/qqp.f32/.json, and faq_texts.jsonl -> faq.f32/.json.

QQP (pairwise): for each threshold t,
  recall     = duplicate pairs with cosine >= t / duplicate pairs   (hits the cache should make)
  false-hit  = non-duplicate pairs with cosine >= t / non-duplicates (wrong answers it would serve)
  precision  = duplicates among pairs >= t, at QQP's base rate
FAQ (cache replay, the cache's actual semantics): seed with canonical questions, replay the
paraphrases and hard negatives in shuffled order; each query HITs the nearest stored question if
cosine >= t (false hit if that question belongs to another intent), otherwise MISSes and is
stored. Averaged over 20 shuffles.

Writes bench/results/phase7/threshold.json and threshold_qqp.png / threshold_faq.png.
Usage: .venv/bin/python tools/analyze_threshold.py
"""

import json
import pathlib
import random

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
EVAL = ROOT / "data/eval"
OUT = ROOT / "bench/results/phase7"
THRESHOLDS = [round(0.50 + 0.01 * i, 2) for i in range(50)]
SHUFFLES = 20


def load(prefix):
    meta = json.loads((EVAL / f"{prefix}.json").read_text())
    vecs = np.fromfile(EVAL / f"{prefix}.f32", dtype="<f4").reshape(meta["count"], meta["dim"])
    return vecs, set(meta["too_long"])


def qqp_curve():
    vecs, too_long = load("qqp")
    data = json.loads((EVAL / "qqp_pairs.json").read_text())
    pairs = [p for p in data["pairs"] if p[0] not in too_long and p[1] not in too_long]
    a = np.array([p[0] for p in pairs])
    b = np.array([p[1] for p in pairs])
    label = np.array([p[2] for p in pairs], dtype=bool)
    cos = np.einsum("ij,ij->i", vecs[a], vecs[b])
    rows = []
    for t in THRESHOLDS:
        above = cos >= t
        tp = int((above & label).sum())
        fp = int((above & ~label).sum())
        rows.append({"threshold": t,
                     "recall": tp / label.sum(),
                     "false_hit_rate": fp / (~label).sum(),
                     "precision": tp / (tp + fp) if tp + fp else 1.0})
    # Rank-based AUC: P(cos(dup) > cos(non-dup)).
    order = np.argsort(cos)
    ranks = np.empty(len(cos))
    ranks[order] = np.arange(1, len(cos) + 1)
    n_pos, n_neg = label.sum(), (~label).sum()
    auc = (ranks[label].sum() - n_pos * (n_pos + 1) / 2) / (n_pos * n_neg)
    return {"source": data["source"], "pairs": len(pairs), "duplicates": int(label.sum()),
            "dropped_too_long": len(data["pairs"]) - len(pairs), "auc": float(auc),
            "cos_dup_median": float(np.median(cos[label])), "cos_nondup_median": float(np.median(cos[~label])),
            "curve": rows}


def faq_replay():
    vecs, _ = load("faq")
    rows = [json.loads(l) for l in (ROOT / "bench/data/faq_eval.jsonl").read_text().splitlines()]
    canon = [i for i, r in enumerate(rows) if r["kind"] == "canonical"]
    rest = [i for i, r in enumerate(rows) if r["kind"] != "canonical"]
    curve = []
    for t in THRESHOLDS:
        hits = correct = false_hits = missed = neg_false = 0
        queries = 0
        for seed in range(SHUFFLES):
            order = rest[:]
            random.Random(seed).shuffle(order)
            stored = list(canon)
            for q in order:
                queries += 1
                sims = vecs[stored] @ vecs[q]
                best = int(np.argmax(sims))
                same = rows[stored[best]]["intent"] == rows[q]["intent"]
                if sims[best] >= t:
                    hits += 1
                    if same:
                        correct += 1
                    else:
                        false_hits += 1
                        neg_false += rows[q]["kind"] == "negative"
                else:
                    if any(rows[s]["intent"] == rows[q]["intent"] for s in stored):
                        missed += 1  # an answer for this intent was cached, but not matched
                    stored.append(q)
        curve.append({"threshold": t, "hit_rate": hits / queries, "false_hit_rate": false_hits / queries,
                      "precision": correct / hits if hits else 1.0, "missed_rate": missed / queries,
                      "negative_false_hit_rate": neg_false / (SHUFFLES * sum(r["kind"] == "negative" for r in rows))})
    return {"questions": len(rows), "shuffles": SHUFFLES, "curve": curve}


def plot(curve, x_keys, labels, title, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7, 4.2))
    ts = [r["threshold"] for r in curve]
    for k, lab in zip(x_keys, labels):
        ax.plot(ts, [r[k] for r in curve], label=lab)
    ax.axvline(0.97, color="grey", linestyle=":", linewidth=1)  # the default threshold
    ax.set_xlabel("cache.similarity_threshold (cosine)")
    ax.set_ylabel("rate")
    ax.set_title(title)
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=120)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    qqp = qqp_curve()
    faq = faq_replay()
    (OUT / "threshold.json").write_text(json.dumps({"qqp": qqp, "faq": faq}, indent=2))
    plot(qqp["curve"], ["recall", "false_hit_rate", "precision"],
         ["recall (duplicates hit)", "false-hit rate (non-duplicates hit)", "precision"],
         f"QQP pairs (n={qqp['pairs']}), all-MiniLM-L6-v2", OUT / "threshold_qqp.png")
    plot(faq["curve"], ["hit_rate", "false_hit_rate", "missed_rate"],
         ["hit rate", "false-hit rate (wrong answer served)", "missed (answer cached, not matched)"],
         f"FAQ cache replay ({faq['questions']} questions, {faq['shuffles']} shuffles)", OUT / "threshold_faq.png")

    print(f"QQP: {qqp['pairs']} pairs, AUC {qqp['auc']:.4f}, median cos dup {qqp['cos_dup_median']:.3f} "
          f"non-dup {qqp['cos_nondup_median']:.3f}")
    print(f"{'t':>5} | {'QQP recall':>10} {'false-hit':>9} {'precision':>9} | {'FAQ hit':>8} {'false-hit':>9} "
          f"{'precision':>9} {'neg FH':>7} {'missed':>7}")
    for q, f in zip(qqp["curve"], faq["curve"]):
        if round(q["threshold"] * 100) % 5 == 0 or q["threshold"] >= 0.80:
            print(f"{q['threshold']:5.2f} | {q['recall']:10.4f} {q['false_hit_rate']:9.4f} {q['precision']:9.4f} | "
                  f"{f['hit_rate']:8.4f} {f['false_hit_rate']:9.4f} {f['precision']:9.4f} "
                  f"{f['negative_false_hit_rate']:7.3f} {f['missed_rate']:7.4f}")


if __name__ == "__main__":
    main()
