#!/usr/bin/env python3
"""Generates the golden files that pin the C++ embedder to the HuggingFace reference.

  tests/data/tokenizer_golden.jsonl  {"text": str, "ids": [int]}        HF BertTokenizerFast ids
  tests/data/embedding_golden.jsonl  {"text": str, "embedding": [float]} sentence-transformers output

Also cross-checks that the pinned ONNX file (scripts/fetch-model.sh) reproduces sentence-transformers
when fed HF token ids, so a C++ mismatch can only come from our tokenizer or pooling.

Usage: .venv/bin/python tools/make_golden.py [--count 3000] [--seed 7]
"""

import argparse
import json
import pathlib
import random
import sys

import numpy as np

REPO = "sentence-transformers/all-MiniLM-L6-v2"
REVISION = "1110a243fdf4706b3f48f1d95db1a4f5529b4d41"  # keep in sync with scripts/fetch-model.sh
ROOT = pathlib.Path(__file__).resolve().parent.parent

# Hand-picked cases for every normalizer / pre-tokenizer / WordPiece rule.
EDGE_CASES = [
    "", " ", "   \t\n\r  ", "a", "A", "Hello, world!", "What's the refund policy?",
    "HELLO WORLD", "don't won't can't", "e-mail co-operate", "U.S.A.", "3.14159 + 2 = 5.14",
    "$100 #hashtag @user 50% a&b a|b a^b a~b `code` <tag> {json: [1, 2]}",
    "naïve café résumé Zürich São Paulo Øresund Ångström façade", "ÀÉÎÕÜ àéîõü ñ Ñ ç Ç ß ẞ",
    "İstanbul ıi", "ΣΊΣΥΦΟΣ σοφός ς", "Москва ПРИВЕТ ёлка Ёж", "مرحبا بالعالم", "שלום עולם",
    "नमस्ते दुनिया", "สวัสดีครับ", "你好世界", "東京は日本の首都です", "ひらがな カタカナ ｶﾀｶﾅ",
    "안녕하세요 세계", "中文English混合text", "𠀀𠀁 𪜀 𫝀 𫠠 𬺰", "Ｆｕｌｌｗｉｄｔｈ　ｓｐａｃｅ",
    "emoji 😀🎉👍🏽 👨‍👩‍👧 🇺🇸", "zero​width‌join‍﻿bom", "soft­hyphen",
    "nbsp here em thin　ideo", "line sep para", "nel\u0085char",
    "ctrl\x01\x02\x07\x1b[0mchars\x7f", "null\x00byte", "repl�char", "privateuse",
    "unassigned\U000e0001tag͸x", "combining é à ö ñ",
    "stacked à́̂̃̄", "mark first ́x", "x" * 100, "y" * 101,
    "supercalifragilisticexpialidocious antidisestablishmentarianism",
    "pneumonoultramicroscopicsilicovolcanoconiosis" * 3, "https://example.com/a/b?c=d&e=f#g",
    "user@example.com", "C++ C# F# .NET node.js", "snake_case camelCase PascalCase kebab-case",
    "“smart quotes” ‘single’ «guillemets» „low“", "dash – em — minus − ellipsis …",
    "¿Qué? ¡Hola! 「括弧」『二重』【隅付き】", "math ∑∫√∞≈≠≤≥ ±×÷", "currency € £ ¥ ₹ ₿ ¢",
    "roman Ⅻ ⅻ superscript ² ³ ¹ fraction ½ ¼", "ligature ﬁ ﬂ ﬀ", "𝐁𝐨𝐥𝐝 𝕯𝖔𝖚𝖇𝖑𝖊",
    "tab\tseparated\tvalues", "multi\n\nline\n\ntext", "  leading and trailing  ",
    "a" + "́" * 50, "ǅ ǈ ǋ titlecase", "Ⓐ ⓑ circled", "Ǆ Ǉ Ǌ digraphs",
    "Ελληνικά ΆΈΉΊΌΎΏ ΐΰ", "Tiếng Việt có dấu", "Türkçe ğüşöç ĞÜŞÖÇ", "Polski żółć ŻÓŁĆ",
    "Čeština řč ŘČ", "Română șț ȘȚ", "Íslenska þð ÞÐ", "Dansk æøå ÆØÅ",
]

# (start, end) code point ranges sampled for random strings: many scripts and categories.
BLOCKS = [
    (0x20, 0x7E), (0x00, 0x1F), (0x7F, 0x9F), (0xA0, 0xFF), (0x100, 0x24F), (0x250, 0x2AF),
    (0x2B0, 0x36F), (0x370, 0x3FF), (0x400, 0x4FF), (0x530, 0x58F), (0x590, 0x5FF), (0x600, 0x6FF),
    (0x900, 0x97F), (0x980, 0x9FF), (0xE00, 0xE7F), (0x10A0, 0x10FF), (0x1100, 0x11FF),
    (0x1E00, 0x1EFF), (0x1F00, 0x1FFF), (0x2000, 0x206F), (0x2070, 0x209F), (0x20A0, 0x20CF),
    (0x2100, 0x214F), (0x2150, 0x218F), (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2460, 0x24FF),
    (0x2500, 0x257F), (0x2600, 0x26FF), (0x2E80, 0x2EFF), (0x3000, 0x303F), (0x3040, 0x30FF),
    (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xAC00, 0xD7A3), (0xE000, 0xF8FF), (0xF900, 0xFAFF),
    (0xFB00, 0xFB4F), (0xFE00, 0xFE0F), (0xFE30, 0xFE4F), (0xFF00, 0xFFEF), (0xFFF0, 0xFFFF),
    (0x1D400, 0x1D7FF), (0x1F300, 0x1F64F), (0x1F900, 0x1F9FF), (0x20000, 0x2A6DF),
    (0x2A700, 0x2CEAF), (0x2F800, 0x2FA1F), (0xE0000, 0xE007F), (0x10FFF0, 0x10FFFF),
]

# Natural-language prompts for the embedding parity gate (FAQ / support / chat style).
SENTENCES = [
    "What is your refund policy?", "How do I get my money back?", "Can I return an item after 30 days?",
    "How do I reset my password?", "I forgot my password, what should I do?",
    "Where can I find my invoice?", "How do I change my billing address?",
    "Do you ship internationally?", "How long does shipping to Canada take?",
    "What payment methods do you accept?", "Can I pay with PayPal?", "Is there a student discount?",
    "How do I cancel my subscription?", "Will I be charged if I cancel mid-month?",
    "How do I contact customer support?", "What are your business hours?",
    "Explain the difference between TCP and UDP.", "What is a mutex and when should I use one?",
    "Write a Python function that reverses a linked list.", "How does HNSW approximate nearest neighbour search work?",
    "Summarize the plot of Hamlet in three sentences.", "Translate 'good morning' into French.",
    "What's the capital of Australia?", "Who wrote Pride and Prejudice?", "How many legs does a spider have?",
    "Give me a recipe for vegan chocolate chip cookies.", "What is the boiling point of water at sea level?",
    "How do vaccines train the immune system?", "Why is the sky blue?", "What causes inflation?",
    "Is it safe to eat raw cookie dough?", "Recommend a good book about distributed systems.",
    "How do I center a div in CSS?", "What does HTTP status 503 mean?", "What is the time complexity of quicksort?",
    "Compare PostgreSQL and MySQL for a write-heavy workload.", "What is a semantic cache for LLMs?",
    "naïve café résumé", "你好，世界！今天天气怎么样？", "Wie spät ist es?", "¿Dónde está la biblioteca?",
    "Привет, как дела?", "これはペンです。", "emoji test 😀🎉 does it embed?", "a", "", "   ",
    "UPPER CASE QUESTION ABOUT REFUNDS", "refund refund refund refund refund",
    "The quick brown fox jumps over the lazy dog. " * 4,
    "Please list every step required to migrate a production database from MySQL 5.7 to PostgreSQL 16, "
    "including schema conversion, data transfer, validation, cut-over planning, rollback strategy, "
    "monitoring, and how to minimise downtime for a service handling ten thousand writes per second. "
    "Assume the application uses an ORM, has stored procedures, and stores timestamps without time zones. "
    "Also explain how to test the migration in staging and what metrics to watch during cut-over.",
]


def random_text(rng: random.Random, vocab: list[str]) -> str:
    parts = []
    for _ in range(rng.randint(1, 12)):
        kind = rng.random()
        if kind < 0.4:
            word = rng.choice(vocab)
            if word.startswith("##") or word.startswith("["):
                word = word.lstrip("#").strip("[]")
            parts.append(word.upper() if rng.random() < 0.2 else word)
        else:
            lo, hi = rng.choice(BLOCKS)
            chars = []
            for _ in range(rng.randint(1, 8)):
                cp = rng.randint(lo, hi)
                if 0xD800 <= cp <= 0xDFFF:
                    continue
                chars.append(chr(cp))
            parts.append("".join(chars))
        parts.append(rng.choice([" ", "", "  ", "\t", "\n", ",", ".", "-"]))
    return "".join(parts)


def write_jsonl(path: pathlib.Path, rows) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=3000, help="random tokenizer strings")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    from transformers import AutoTokenizer
    from sentence_transformers import SentenceTransformer
    import onnxruntime as ort

    tok = AutoTokenizer.from_pretrained(REPO, revision=REVISION, use_fast=True)
    vocab = sorted(tok.get_vocab(), key=tok.get_vocab().get)
    rng = random.Random(args.seed)

    texts = EDGE_CASES + SENTENCES + [random_text(rng, vocab) for _ in range(args.count)]
    rows = [{"text": t, "ids": tok(t, add_special_tokens=True, truncation=False)["input_ids"]} for t in texts]
    write_jsonl(ROOT / "tests/data/tokenizer_golden.jsonl", rows)
    print(f"tokenizer golden: {len(rows)} strings")

    model = SentenceTransformer(REPO, revision=REVISION, device="cpu")
    max_len = model.max_seq_length
    sentences = [s for s in SENTENCES + EDGE_CASES if len(tok(s)["input_ids"]) <= max_len]
    ref = model.encode(sentences, batch_size=16, convert_to_numpy=True, normalize_embeddings=True)

    # Cross-check: the pinned ONNX file + HF ids + mean pooling must reproduce sentence-transformers.
    onnx_path = ROOT / "models/all-MiniLM-L6-v2.onnx"
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    worst = 1.0
    for s, r in zip(sentences, ref):
        ids = np.array([tok(s)["input_ids"]], dtype=np.int64)
        mask = np.ones_like(ids)
        hidden = sess.run(None, {"input_ids": ids, "attention_mask": mask, "token_type_ids": np.zeros_like(ids)})[0]
        v = hidden[0].mean(axis=0)
        v /= np.linalg.norm(v)
        worst = min(worst, float(np.dot(v, r)))
    print(f"onnx vs sentence-transformers: min cosine {worst:.7f} over {len(sentences)} sentences")
    if worst < 0.9999:
        print("ERROR: pinned ONNX does not reproduce sentence-transformers", file=sys.stderr)
        return 1

    write_jsonl(ROOT / "tests/data/embedding_golden.jsonl",
                ({"text": s, "embedding": [float(f"{x:.8g}") for x in v]} for s, v in zip(sentences, ref)))
    print(f"embedding golden: {len(sentences)} sentences (max_seq_length {max_len})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
