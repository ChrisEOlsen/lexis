#!/usr/bin/env python3
"""Phase 0: where BM25 ranks the first correct passage (8-word shingle match).
Decides whether reranking is worth building; reads raw/*.json + passages/retrieval TSVs."""

import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_common import shingles

CUTOFFS = (1, 3, 5, 12, 40)


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: phase0_score.py <raw_dir> <passages.tsv> <retrieval.tsv>")
    raw_dir, passages_tsv, retrieval_tsv = sys.argv[1:4]

    # Questions and gold passages, deduplicated by question text.
    gold_by_question = collections.OrderedDict()
    for name in sorted(os.listdir(raw_dir)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(raw_dir, name)) as handle:
            for row in json.load(handle):
                question = (row.get("question") or "").strip()
                if not question:
                    continue
                bucket = gold_by_question.setdefault(question, set())
                for doc in row.get("documents") or []:
                    if doc and doc.strip():
                        bucket |= shingles(doc)

    questions = list(gold_by_question.keys())

    # Passage id -> shingles.
    passage_shingles = {}
    with open(passages_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t", 3)
            if len(parts) < 4:
                continue
            passage_shingles[int(parts[0])] = shingles(parts[3])

    # Retrieval results.
    ranked = collections.defaultdict(list)
    with open(retrieval_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 4:
                continue
            ranked[int(parts[0])].append((int(parts[1]), int(parts[2])))

    first_hit = []          # rank of the first correct passage, or None
    no_gold = 0             # questions whose gold text never made it into the corpus
    for index, question in enumerate(questions):
        gold = gold_by_question[question]
        if not gold:
            no_gold += 1
            first_hit.append(None)
            continue
        hit = None
        for rank, passage_id in sorted(ranked.get(index, [])):
            if gold & passage_shingles.get(passage_id, set()):
                hit = rank
                break
        first_hit.append(hit)

    total = len(questions)
    found = [r for r in first_hit if r is not None]

    print(f"\nquestions:            {total}")
    print(f"correct passage found within top 40: {len(found)}  ({len(found)/total:.1%})")
    print(f"never found in top 40:               {total - len(found)}  ({(total-len(found))/total:.1%})")
    if no_gold:
        print(f"  (of which {no_gold} had no gold text at all)")

    print("\nrecall@K -- share of questions with a correct passage by rank K")
    for k in CUTOFFS:
        hits = sum(1 for r in found if r <= k)
        print(f"  recall@{k:<3} {hits/total:6.1%}   ({hits}/{total})")

    mrr = sum(1.0 / r for r in found) / total if total else 0.0
    print(f"\nMRR (first correct passage): {mrr:.3f}")

    # Headroom: answer present below the cutoff, so reordering could help.
    for send in (5, 12):
        recoverable = sum(1 for r in found if r > send)
        print(
            f"\nsending top {send}: {recoverable} questions ({recoverable/total:.1%}) have a correct "
            f"passage ranked below {send} but within 40."
        )
        print(f"  -> that is the maximum any reranker could add at K={send}.")

    print("\nrank distribution of the first correct passage")
    buckets = collections.Counter()
    for r in found:
        if r <= 3:
            buckets["1-3"] += 1
        elif r <= 5:
            buckets["4-5"] += 1
        elif r <= 12:
            buckets["6-12"] += 1
        else:
            buckets["13-40"] += 1
    for label in ("1-3", "4-5", "6-12", "13-40"):
        count = buckets[label]
        bar = "#" * int(50 * count / max(1, len(found)))
        print(f"  {label:>6}  {count:5d}  {bar}")
    print(f"  {'none':>6}  {total-len(found):5d}")


if __name__ == "__main__":
    main()
