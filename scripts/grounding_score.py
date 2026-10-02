#!/usr/bin/env python3
"""Grounding attribution for a lexis_eval run with passages: gold_sent + NLI support.
Usage: grounding_score.py <raw_dir> <run_tsv> <stopwords> <out.tsv> (needs venv; ~0.7GB judge)."""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_common import content_words, looks_like_refusal, shingles

GOLD_SENT_MIN = 0.35  # fraction of a gold doc's shingles that must arrive
SUPPORT_MIN = 0.5
WEAK_COVERAGE = 0.25
HHEM_BATCH = 64


def main():
    raw_dir, run_tsv, stopwords_path, out_path = sys.argv[1:5]

    with open(stopwords_path) as fh:
        stopwords = {l.strip().lower() for l in fh if l.strip()}

    # Per-doc (not unioned) gold shingle sets: one retrieved gold doc suffices.
    refs, gold_docs = {}, {}
    for name in sorted(os.listdir(raw_dir)):
        if not name.endswith(".json"):
            continue
        for row in json.load(open(os.path.join(raw_dir, name))):
            q = " ".join((row.get("question") or "").split())
            if not q:
                continue
            refs.setdefault(q, []).append((row.get("response") or "").strip())
            bucket = gold_docs.setdefault(q, [])
            for doc in row.get("documents") or []:
                if doc and doc.strip():
                    bucket.append(shingles(doc))

    rows = []
    with open(run_tsv) as fh:
        fh.readline()
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 8:
                continue
            idx, tool, _npass, _secs, _ok, question, answer, passages_json = parts[:8]
            try:
                passages = json.loads(passages_json)
            except json.JSONDecodeError:
                passages = []
            rows.append(
                {
                    "idx": int(idx),
                    "tool": tool,
                    "question": " ".join(question.split()),
                    "answer": answer,
                    "passages": [p for p in passages if p and p.strip()],
                }
            )

    print(f"scoring {len(rows)} rows", file=sys.stderr)

    # -- deterministic measures first --
    for row in rows:
        q = row["question"]
        sent_shingles = set()
        for p in row["passages"]:
            sent_shingles |= shingles(p)

        row["refusal"] = looks_like_refusal(row["answer"])

        best_frac = 0.0
        for doc_sh in gold_docs.get(q, []):
            if doc_sh:
                frac = len(doc_sh & sent_shingles) / len(doc_sh)
                best_frac = max(best_frac, frac)
        row["gold_frac"] = best_frac
        row["gold_sent"] = best_frac >= GOLD_SENT_MIN

        best_cov = 0.0
        ans_words = content_words(row["answer"], stopwords)
        for ref in refs.get(q, []):
            ref_words = content_words(ref, stopwords)
            if ref_words:
                best_cov = max(best_cov, len(ans_words & ref_words) / len(ref_words))
        row["coverage"] = best_cov

    # -- NLI support, batched over every (passage, answer) pair --
    import torch
    from transformers import AutoModelForSequenceClassification, AutoTokenizer

    judge_name = "MoritzLaurer/DeBERTa-v3-base-mnli-fever-anli"
    tokenizer = AutoTokenizer.from_pretrained(judge_name)
    model = AutoModelForSequenceClassification.from_pretrained(judge_name)
    model.eval()
    # MPS is ~100x CPU speed here (0.6s vs ~70s per batch on Apple Silicon).
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    model.to(device)
    entailment_idx = model.config.label2id["entailment"]

    pairs, owners = [], []
    for row in rows:
        if row["tool"] != "search" or not row["passages"]:
            row["support"] = None
            continue
        row["support"] = 0.0
        for p in row["passages"]:
            pairs.append((p, row["answer"]))
            owners.append(row)

    for start in range(0, len(pairs), HHEM_BATCH):
        batch = pairs[start : start + HHEM_BATCH]
        # Fixed 512: MPS recompiles a kernel per shape, so "longest" recompiles every batch.
        inputs = tokenizer(
            [p for p, _ in batch], [a for _, a in batch],
            return_tensors="pt", padding="max_length", truncation=True, max_length=512,
        ).to(device)
        with torch.no_grad():
            probs = torch.softmax(model(**inputs).logits, dim=-1)[:, entailment_idx].cpu()
        for owner, score in zip(owners[start : start + HHEM_BATCH], probs):
            owner["support"] = max(owner["support"], float(score))
        print(f"  nli {min(start + HHEM_BATCH, len(pairs))}/{len(pairs)}", file=sys.stderr, flush=True)

    # -- per-row output --
    with open(out_path, "w") as out:
        out.write("idx\ttool\trefusal\tgold_sent\tgold_frac\tsupport\tcoverage\tquestion\n")
        for row in rows:
            support = "" if row["support"] is None else f"{row['support']:.3f}"
            out.write(
                f"{row['idx']}\t{row['tool']}\t{int(row['refusal'])}\t{int(row['gold_sent'])}\t"
                f"{row['gold_frac']:.3f}\t{support}\t{row['coverage']:.3f}\t{row['question']}\n"
            )

    # -- summary --
    search = [r for r in rows if r["tool"] == "search"]
    answered = [r for r in search if not r["refusal"]]
    weak = [r for r in answered if r["coverage"] < WEAK_COVERAGE]
    refused = [r for r in search if r["refusal"]]

    def pct(part, whole):
        return f"{100.0 * len(part) / len(whole):.1f}%" if whole else "n/a"

    gold_ok = [r for r in search if r["gold_sent"]]
    supported = [r for r in answered if r["support"] is not None and r["support"] >= SUPPORT_MIN]

    print(f"\nsearch rows: {len(search)}  answered: {len(answered)}  refusals: {len(refused)}")
    print(f"gold_sent (right passage reached the model): {len(gold_ok)}/{len(search)} = {pct(gold_ok, search)}")
    print(f"supported answers (NLI  >= {SUPPORT_MIN}):    {len(supported)}/{len(answered)} = {pct(supported, answered)}")

    print("\nattribution of the weak answers (coverage < 25%):")
    weak_retrieval = [r for r in weak if not r["gold_sent"]]
    weak_generation = [r for r in weak if r["gold_sent"]]
    weak_gen_unsupported = [r for r in weak_generation if r["support"] is not None and r["support"] < SUPPORT_MIN]
    print(f"  weak total:               {len(weak)}")
    print(f"  retrieval never delivered: {len(weak_retrieval)}  ({pct(weak_retrieval, weak)})")
    print(f"  model had it, answered weakly: {len(weak_generation)}  ({pct(weak_generation, weak)})")
    print(f"    ...of which the NLI judge also says unsupported: {len(weak_gen_unsupported)}")

    print("\nattribution of the refusals:")
    ref_retrieval = [r for r in refused if not r["gold_sent"]]
    print(f"  refusals total: {len(refused)}; gold never arrived: {len(ref_retrieval)}; "
          f"gold arrived but model refused: {len(refused) - len(ref_retrieval)}")


if __name__ == "__main__":
    main()
