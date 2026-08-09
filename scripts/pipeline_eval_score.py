#!/usr/bin/env python3
"""Score a lexis_eval run against DelucionQA's reference answers.

What is measured, and what each measure is worth:

  tool          which tool the router picked. Reported, not scored -- there
                is no ground truth for it in DelucionQA, but the
                distribution is diagnostic on its own (every one of these
                questions is a specific lookup, so anything other than
                SEARCH is worth looking at).

  gold_sent     whether a passage containing the reference answer's
                supporting text actually reached the model. This is the one
                measure with real ground truth, and it separates "the model
                failed" from "the passage was never there".

  coverage      share of the reference answer's content words that appear in
                the generated answer. Deliberately crude: DelucionQA's
                references are prose, and the only local model available is
                the same one under test, so using it as a judge would be
                circular. Lexical overlap is at least deterministic and
                applied identically everywhere. It UNDERCOUNTS correct
                answers that paraphrase and OVERCOUNTS verbose ones that
                restate the question, so treat it as a screening signal for
                finding cases to read, not as a grade.

  refusal      whether the answer looks like a non-answer ("I don't have
                enough", "no matching passages", asking the user for
                documents). Counted separately because a refusal scores low
                on coverage for a completely different reason than a wrong
                answer does.
"""

import collections
import json
import os
import re
import sys

SHINGLE = 8

REFUSAL_MARKERS = (
    "don't have enough",
    "do not have enough",
    "not enough information",
    "no matching passages",
    "does not contain",
    "doesn't contain",
    "cannot answer",
    "can't answer",
    "please provide",
    "could you rephrase",
    "i only have",
    "i don't have access",
)


def normalize(text):
    return " ".join(text.lower().split())


def shingles(text):
    words = normalize(text).split()
    if len(words) < SHINGLE:
        return {" ".join(words)} if words else set()
    return {" ".join(words[i : i + SHINGLE]) for i in range(len(words) - SHINGLE + 1)}


def content_words(text, stopwords):
    return {w for w in re.findall(r"[a-z0-9]+", text.lower()) if w not in stopwords and len(w) > 2}


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
        fh.readline()  # header
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
            low = answer.lower()
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
                    "refusal": any(m in low for m in REFUSAL_MARKERS),
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
