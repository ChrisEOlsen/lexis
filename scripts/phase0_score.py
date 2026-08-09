#!/usr/bin/env python3
"""Phase 0: how far down the results does the correct passage actually sit?

Answers one question, with no language model involved: for each DelucionQA
question, where does BM25 rank a passage that genuinely contains the answer?

That single distribution decides whether reranking is worth building:

  - correct passage usually already in the top 5   -> ordering is not the
                                                      problem; build nothing
  - usually within top 40 but below top 5          -> reordering would fix a
                                                      lot; build the selection
                                                      step
  - often absent from the top 40 entirely          -> no reordering can help;
                                                      retrieval itself is the
                                                      problem

Gold matching. DelucionQA gives the passages that were retrieved for each
question; LEXIS re-chunks those into ~118-token windows that do not align with
the originals, so exact string equality would find nothing. A retrieved chunk
counts as correct when it shares at least one 8-word shingle with a gold
passage: long enough that an accidental match is implausible, short enough to
survive the chunk boundaries falling in different places.

Reads:  data/eval/delucionqa/raw/*.json   (questions + gold passages)
        a passages TSV dumped from the ingested corpus
        the TSV emitted by phase0_retrieval
"""

import collections
import json
import os
import sys

SHINGLE = 8
CUTOFFS = (1, 3, 5, 12, 40)


def normalize(text):
    return " ".join(text.lower().split())


def shingles(text):
    words = normalize(text).split()
    if len(words) < SHINGLE:
        return {" ".join(words)} if words else set()
    return {" ".join(words[i : i + SHINGLE]) for i in range(len(words) - SHINGLE + 1)}


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: phase0_score.py <raw_dir> <passages.tsv> <retrieval.tsv>")
    raw_dir, passages_tsv, retrieval_tsv = sys.argv[1:4]

    # -- questions and their gold passages, deduplicated by question text --
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

    # -- passage id -> its shingles --
    passage_shingles = {}
    with open(passages_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t", 3)
            if len(parts) < 4:
                continue
            passage_shingles[int(parts[0])] = shingles(parts[3])

    # -- retrieval results --
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

    # The headroom: questions where reordering could help, because the answer
    # is present in the candidate set but below where we currently cut.
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
