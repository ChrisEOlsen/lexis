#!/usr/bin/env python3
"""Pick a stratified question sample for the 5-vs-12 A/B, using Phase 0's ranks.

Running all 913 questions through two generation configs costs about five
hours. Almost all of that would be wasted: Phase 0 found a correct passage at
rank 1 for 82% of questions, and where gold sits at rank 1 both configs contain
it, so both will tend to answer the same way.

The questions that actually discriminate are stratified by where gold ranks:

  1-3    both configs contain gold -> isolates whether the 7 extra distractors
         in the 12-passage config hurt
  4-5    both contain gold, gold near the 5-passage boundary
  6-12   ONLY the 12-passage config contains gold -> measures what the recall
         drop from 97.0% to 93.5% actually costs in answers
  13+/none  neither contains gold; excluded, since neither config can answer
         and including them would only add noise to both arms equally

Sampling is deterministic (every Nth within a stratum, no RNG) so the run can
be repeated or extended without reshuffling what was already measured.
"""

import collections
import json
import os
import sys

SHINGLE = 8
# Stratum -> how many questions to take. The 6-12 band is small and is the
# whole point of the experiment, so it is sampled hardest.
QUOTAS = {"1-3": 25, "4-5": 10, "6-12": 25}


def normalize(text):
    return " ".join(text.lower().split())


def shingles(text):
    words = normalize(text).split()
    if len(words) < SHINGLE:
        return {" ".join(words)} if words else set()
    return {" ".join(words[i : i + SHINGLE]) for i in range(len(words) - SHINGLE + 1)}


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
