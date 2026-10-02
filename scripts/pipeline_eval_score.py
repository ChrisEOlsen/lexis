#!/usr/bin/env python3
"""Score a lexis_eval run: tool distribution, gold_sent, lexical coverage, refusals.
Coverage is a crude screening signal (undercounts paraphrase), not a grade."""

import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_common import content_words, looks_like_refusal, shingles


def main():
    raw_dir, passages_tsv, results_tsv, stopwords_path = sys.argv[1:5]

    with open(stopwords_path) as fh:
        stopwords = {l.strip().lower() for l in fh if l.strip()}

    refs, gold = {}, {}
    for name in sorted(os.listdir(raw_dir)):
        if not name.endswith(".json"):
            continue
        for row in json.load(open(os.path.join(raw_dir, name))):
            q = " ".join((row.get("question") or "").split())
            if not q:
                continue
            refs.setdefault(q, []).append((row.get("response") or "").strip())
            bucket = gold.setdefault(q, set())
            for doc in row.get("documents") or []:
                if doc and doc.strip():
                    bucket |= shingles(doc)

    passage_shingles = {}
    with open(passages_tsv) as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t", 3)
            if len(parts) >= 4:
                passage_shingles[int(parts[0])] = shingles(parts[3])

    rows = []
    with open(results_tsv) as fh:
        fh.readline()
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 7:
                continue
            idx, tool, npass, secs, ok, question, answer = parts[:7]
            q = " ".join(question.split())
            ans_words = content_words(answer, stopwords)
            best = 0.0
            for ref in refs.get(q, []):
                rw = content_words(ref, stopwords)
                if rw:
                    best = max(best, len(rw & ans_words) / len(rw))
            rows.append(
                {
                    "i": int(idx),
                    "tool": tool,
                    "npass": int(npass),
                    "secs": float(secs),
                    "ok": ok == "1",
                    "q": q,
                    "a": answer,
                    "coverage": best,
                    "refusal": looks_like_refusal(answer),
                    "known": q in refs,
                }
            )

    n = len(rows)
    print(f"questions run: {n}")
    unknown = [r for r in rows if not r["known"]]
    if unknown:
        print(f"  WARNING: {len(unknown)} had no reference answer (text mismatch?)")

    print("\ntool distribution")
    for tool, count in collections.Counter(r["tool"] for r in rows).most_common():
        secs = [r["secs"] for r in rows if r["tool"] == tool]
        print(f"  {tool:<8} {count:3d}  ({count/n:5.1%})   mean {sum(secs)/len(secs):5.1f}s")

    failed = [r for r in rows if not r["ok"]]
    refusals = [r for r in rows if r["refusal"]]
    print(f"\npipeline failures (ok=0): {len(failed)}")
    print(f"refusal-shaped answers:   {len(refusals)}")

    cov = [r["coverage"] for r in rows]
    cov_sorted = sorted(cov)
    print(f"\ncoverage vs reference answer")
    print(f"  mean   {sum(cov)/n:.1%}")
    print(f"  median {cov_sorted[n//2]:.1%}")
    for lo, hi in ((0.0, 0.25), (0.25, 0.5), (0.5, 0.75), (0.75, 1.01)):
        c = sum(1 for x in cov if lo <= x < hi)
        print(f"  {int(lo*100):3d}-{int(hi*100):3d}%  {c:3d}  {'#'*int(40*c/n)}")

    secs = [r["secs"] for r in rows]
    print(f"\nlatency: mean {sum(secs)/n:.1f}s  median {sorted(secs)[n//2]:.1f}s  "
          f"min {min(secs):.1f}s  max {max(secs):.1f}s  total {sum(secs)/60:.1f} min")

    print("\nweakest 8 by coverage (these are the ones to read):")
    for r in sorted(rows, key=lambda r: r["coverage"])[:8]:
        print(f"\n  [{r['i']}] {r['coverage']:.0%}  tool={r['tool']} passages={r['npass']} "
              f"{'REFUSAL' if r['refusal'] else ''}")
        print(f"    Q: {r['q'][:110]}")
        print(f"    A: {r['a'][:220]}")
        ref = refs.get(r["q"], [""])[0]
        print(f"    R: {ref[:220]}")


if __name__ == "__main__":
    main()
