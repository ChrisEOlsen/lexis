#!/usr/bin/env python3
"""Stratified 5-vs-12 A/B sample by Phase 0 gold rank; deterministic (every Nth, no RNG).
Strata 1-3/4-5/6-12 discriminate the arms; 13+/none excluded (neither can answer)."""

import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_common import shingles

# Stratum -> quota. The 6-12 band is the point of the experiment, sampled hardest.
QUOTAS = {"1-3": 25, "4-5": 10, "6-12": 25}


def stratum_of(rank):
    if rank is None:
        return None
    if rank <= 3:
        return "1-3"
    if rank <= 5:
        return "4-5"
    if rank <= 12:
        return "6-12"
    return None


def main():
    raw_dir, passages_tsv, retrieval_tsv, questions_txt, out_questions, out_meta = sys.argv[1:7]

    gold = collections.OrderedDict()
    for name in sorted(os.listdir(raw_dir)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(raw_dir, name)) as handle:
            for row in json.load(handle):
                question = " ".join((row.get("question") or "").split())
                if not question:
                    continue
                bucket = gold.setdefault(question, set())
                for doc in row.get("documents") or []:
                    if doc and doc.strip():
                        bucket |= shingles(doc)

    passage_shingles = {}
    with open(passages_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t", 3)
            if len(parts) >= 4:
                passage_shingles[int(parts[0])] = shingles(parts[3])

    ranked = collections.defaultdict(list)
    with open(retrieval_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 4:
                ranked[int(parts[0])].append((int(parts[1]), int(parts[2])))

    with open(questions_txt) as handle:
        questions = [l.rstrip("\n") for l in handle if l.strip()]

    by_stratum = collections.defaultdict(list)
    for index, question in enumerate(questions):
        gold_shingles = gold.get(question, set())
        hit = None
        if gold_shingles:
            for rank, passage_id in sorted(ranked.get(index, [])):
                if gold_shingles & passage_shingles.get(passage_id, set()):
                    hit = rank
                    break
        name = stratum_of(hit)
        if name:
            by_stratum[name].append((index, question, hit))

    chosen = []
    for name, quota in QUOTAS.items():
        pool = by_stratum[name]
        if not pool:
            continue
        step = max(1, len(pool) // quota)
        picked = pool[::step][:quota]
        chosen.extend((name, *entry) for entry in picked)
        print(f"  {name:>5}: {len(picked)} of {len(pool)} available")

    chosen.sort(key=lambda row: row[1])
    with open(out_questions, "w") as handle:
        handle.write("\n".join(row[2] for row in chosen) + "\n")
    with open(out_meta, "w") as handle:
        for local_index, (name, original_index, question, rank) in enumerate(chosen):
            handle.write(f"{local_index}\t{name}\t{rank}\t{original_index}\n")

    print(f"  total: {len(chosen)} questions -> {len(chosen) * 2} generations")


if __name__ == "__main__":
    main()
