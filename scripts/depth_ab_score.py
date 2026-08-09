#!/usr/bin/env python3
"""Score the 5-vs-12 A/B run.

Two things are measured per answer:

  gold_sent   whether a passage containing the reference answer's supporting
              text actually reached the model. This separates "the model
              failed" from "the passage was never there", which is the whole
              reason the harness records which passage ids it sent.

  coverage    what share of the reference answer's content words appear in the
              generated answer. Crude on purpose: DelucionQA's references are
              prose, and the only local model available is the same 2B one that
              has already shown instability, so using it as a judge would be
              unsound. A lexical measure is at least deterministic and applied
              identically to both arms, which is what a comparison needs. It
              will undercount correct answers that paraphrase, so read the
              DIFFERENCE between arms, not the absolute level.

Results are broken out by the stratum each question was sampled from, since
the arms are expected to differ only where gold sits between the two cutoffs.
"""

import collections
import json
import os
import re
import sys

SHINGLE = 8


def normalize(text):
    return " ".join(text.lower().split())


def shingles(text):
    words = normalize(text).split()
    if len(words) < SHINGLE:
        return {" ".join(words)} if words else set()
    return {" ".join(words[i : i + SHINGLE]) for i in range(len(words) - SHINGLE + 1)}


def content_words(text, stopwords):
    words = re.findall(r"[a-z0-9]+", text.lower())
    return {w for w in words if w not in stopwords and len(w) > 2}


def main():
    raw_dir, passages_tsv, questions_txt, meta_tsv, results_tsv, stopwords_path = sys.argv[1:7]

    with open(stopwords_path) as handle:
        stopwords = {l.strip().lower() for l in handle if l.strip()}

    references, gold_shingles = {}, {}
    for name in sorted(os.listdir(raw_dir)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(raw_dir, name)) as handle:
            for row in json.load(handle):
                question = " ".join((row.get("question") or "").split())
                if not question:
                    continue
                references.setdefault(question, []).append((row.get("response") or "").strip())
                bucket = gold_shingles.setdefault(question, set())
                for doc in row.get("documents") or []:
                    if doc and doc.strip():
                        bucket |= shingles(doc)

    passage_shingles = {}
    with open(passages_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t", 3)
            if len(parts) >= 4:
                passage_shingles[int(parts[0])] = shingles(parts[3])

    with open(questions_txt) as handle:
        questions = [l.rstrip("\n") for l in handle if l.strip()]

    stratum = {}
    with open(meta_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                stratum[int(parts[0])] = (parts[1], int(parts[2]))

    rows = []
    with open(results_tsv) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 7:
                continue
            index, config, n_sent = int(parts[0]), int(parts[1]), int(parts[2])
            ids = [int(x) for x in parts[3].split(",") if x]
            prompt_tokens, seconds, answer = int(parts[4]), float(parts[5]), parts[6]
            question = questions[index]

            sent = set()
            for pid in ids:
                sent |= passage_shingles.get(pid, set())
            gold_sent = bool(gold_shingles.get(question, set()) & sent)

            answer_words = content_words(answer, stopwords)
            best = 0.0
            for ref in references.get(question, []):
                ref_words = content_words(ref, stopwords)
                if ref_words:
                    best = max(best, len(ref_words & answer_words) / len(ref_words))

            rows.append(
                {
                    "index": index,
                    "config": config,
                    "n_sent": n_sent,
                    "gold_sent": gold_sent,
                    "coverage": best,
                    "prompt_tokens": prompt_tokens,
                    "seconds": seconds,
                    "chars": len(answer),
                    "stratum": stratum.get(index, ("?", 0))[0],
                    "gold_rank": stratum.get(index, ("?", 0))[1],
                }
            )

    def summarize(subset, label):
        if not subset:
            return
        by_config = collections.defaultdict(list)
        for row in subset:
            by_config[row["config"]].append(row)
        print(f"\n{label}  (n={len(by_config[12])} questions)")
        print(f"  {'cfg':>4} {'gold sent':>10} {'coverage':>9} {'answer len':>11} {'prompt tok':>11} {'sec':>6}")
        for config in sorted(by_config, reverse=True):
            group = by_config[config]
            n = len(group)
            print(
                f"  {config:>4} {sum(r['gold_sent'] for r in group)/n:>9.0%} "
                f"{sum(r['coverage'] for r in group)/n:>9.1%} "
                f"{sum(r['chars'] for r in group)/n:>11.0f} "
                f"{sum(r['prompt_tokens'] for r in group)/n:>11.0f} "
                f"{sum(r['seconds'] for r in group)/n:>6.1f}"
            )

    summarize(rows, "ALL")
    for name in ("1-3", "4-5", "6-12"):
        summarize([r for r in rows if r["stratum"] == name], f"gold at rank {name}")

    # Per-question head-to-head, so a mean isn't hiding offsetting swings.
    paired = collections.defaultdict(dict)
    for row in rows:
        paired[row["index"]][row["config"]] = row
    better5 = better12 = tied = 0
    for index, arms in paired.items():
        if 5 not in arms or 12 not in arms:
            continue
        delta = arms[5]["coverage"] - arms[12]["coverage"]
        if delta > 0.05:
            better5 += 1
        elif delta < -0.05:
            better12 += 1
        else:
            tied += 1
    print(f"\nhead-to-head (coverage, 5pp threshold): 5 better {better5} | 12 better {better12} | tied {tied}")


if __name__ == "__main__":
    main()
