#!/usr/bin/env python3
"""Fetch DelucionQA into data/eval/delucionqa/ for hands-on testing of LEXIS.

DelucionQA is a retrieval-QA dataset built on the Jeep 2023 Gladiator owner's
manual: specific, lookup-style questions against a long technical manual --
structurally the same shape as the NY driver's manual this project has been
tested against, but with human-annotated reference answers.

It is fetched from the `delucionqa` subset of the RAGBench collection
(https://huggingface.co/datasets/galileo-ai/ragbench) through HuggingFace's
datasets-server JSON API rather than the parquet files, so this script needs
no pandas/pyarrow -- only the standard library.

What it writes:

  corpus/*.txt      the manual passages, deduplicated, split across several
                    files so a group has more than one document to retrieve
                    across. These are what you ingest into LEXIS.
  questions.md      every unique question with its reference answer, readable
                    side-by-side while using the app.
  starter.md        a 30-question subset to work through first.
  raw/*.json        the untouched API rows, for the automated harness later.

IMPORTANT -- what this corpus is, and is not. `documents` in DelucionQA are the
passages that were RETRIEVED for each question, not the full owner's manual. So
the corpus assembled here is the union of gold contexts: every question is
answerable from it, and there are far fewer distractor sections than the real
manual has. Retrieval will look easier here than against a complete document.
That is the right trade for manual testing (you can always tell whether a wrong
answer was the model's fault, since the supporting text is definitely present),
but do not read good scores here as a measure of retrieval quality at scale.
"""

import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

DATASET = "galileo-ai/ragbench"
CONFIG = "delucionqa"
SPLITS = ("train", "validation", "test")
PAGE = 100  # datasets-server caps `length` at 100 rows per request

OUT_ROOT = os.path.join("data", "eval", "delucionqa")
PASSAGES_PER_FILE = 25
STARTER_COUNT = 30


def fetch_rows(split):
    """Page through one split, returning the list of row dicts."""
    rows, offset = [], 0
    while True:
        params = urllib.parse.urlencode(
            {"dataset": DATASET, "config": CONFIG, "split": split, "offset": offset, "length": PAGE}
        )
        url = f"https://datasets-server.huggingface.co/rows?{params}"
        try:
            with urllib.request.urlopen(url, timeout=60) as resp:
                payload = json.load(resp)
        except urllib.error.HTTPError as exc:
            sys.exit(f"HTTP {exc.code} fetching {split} at offset {offset}: {exc.reason}")
        except urllib.error.URLError as exc:
            sys.exit(f"network error fetching {split} at offset {offset}: {exc.reason}")

        batch = payload.get("rows", [])
        if not batch:
            break
        rows.extend(r["row"] for r in batch)
        offset += len(batch)
        print(f"  {split}: {offset} rows", end="\r", flush=True)
        if offset >= payload.get("num_rows_total", 0):
            break
    print(f"  {split}: {len(rows)} rows      ")
    return rows


def slugify(text, limit=60):
    slug = re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")
    return slug[:limit] or "question"


def main():
    os.makedirs(os.path.join(OUT_ROOT, "corpus"), exist_ok=True)
    os.makedirs(os.path.join(OUT_ROOT, "raw"), exist_ok=True)

    all_rows = []
    for split in SPLITS:
        rows = fetch_rows(split)
        # Compact separators: these files carry every annotation column and
        # are only ever read by a program, so pretty-printing them tripled
        # the on-disk size for nothing.
        with open(os.path.join(OUT_ROOT, "raw", f"{split}.json"), "w") as handle:
            json.dump(rows, handle, separators=(",", ":"))
        all_rows.extend(rows)

    # -- corpus: distinct passages, with contained fragments removed --
    #
    # Exact-match dedup alone is not enough. DelucionQA's `documents` are
    # retrieval chunks with overlapping windows, so the same manual text shows
    # up both as its own short passage and embedded inside longer ones. Left
    # in, that text would be indexed several times over, inflating its BM25
    # term frequencies and scattering provenance across near-identical chunks.
    #
    # Longest first, then drop any passage already contained in one that was
    # kept. O(n^2) over ~1k passages runs instantly and needs no dependency;
    # it catches whole containment, not near-duplicate paraphrase.
    seen, unique = set(), []
    for row in all_rows:
        for doc in row.get("documents") or []:
            text = (doc or "").strip()
            if text and text not in seen:
                seen.add(text)
                unique.append(text)

    unique.sort(key=len, reverse=True)
    passages, contained = [], 0
    for text in unique:
        if any(text in kept for kept in passages):
            contained += 1
            continue
        passages.append(text)
    print(f"  passages: {len(unique)} distinct, {contained} contained in a longer one, {len(passages)} kept")

    for index in range(0, len(passages), PASSAGES_PER_FILE):
        part = index // PASSAGES_PER_FILE + 1
        path = os.path.join(OUT_ROOT, "corpus", f"gladiator_manual_part{part:02d}.txt")
        with open(path, "w") as handle:
            handle.write(
                f"Jeep 2023 Gladiator owner's manual -- excerpts (part {part})\n"
                f"Source: DelucionQA via RAGBench. Passages are manual excerpts, not the full manual.\n\n"
            )
            handle.write("\n\n".join(passages[index : index + PASSAGES_PER_FILE]))
            handle.write("\n")

    file_count = (len(passages) + PASSAGES_PER_FILE - 1) // PASSAGES_PER_FILE

    # -- questions: deduplicated, keeping every distinct reference answer --
    by_question = {}
    for row in all_rows:
        question = (row.get("question") or "").strip()
        answer = (row.get("response") or "").strip()
        if not question:
            continue
        entry = by_question.setdefault(question, {"answers": [], "adherent": 0, "total": 0})
        if answer and answer not in entry["answers"]:
            entry["answers"].append(answer)
        entry["total"] += 1
        if row.get("adherence_score"):
            entry["adherent"] += 1

    questions = sorted(by_question.items(), key=lambda kv: kv[0].lower())

    def write_questions(path, items, title, preamble):
        with open(path, "w") as handle:
            handle.write(f"# {title}\n\n{preamble}\n\n")
            for number, (question, meta) in enumerate(items, start=1):
                handle.write(f"## {number}. {question}\n\n")
                for answer in meta["answers"]:
                    handle.write(f"- {answer}\n")
                if len(meta["answers"]) > 1:
                    handle.write(
                        f"\n_({len(meta['answers'])} reference answers recorded for this question.)_\n"
                    )
                handle.write("\n")

    preamble = (
        "Reference answers are GPT-3.5 responses that human annotators judged against the\n"
        "retrieved manual passages -- treat them as a guide to what a correct answer covers,\n"
        "not as exact strings to match. A question can carry more than one reference answer\n"
        "where the dataset recorded several generations."
    )
    write_questions(
        os.path.join(OUT_ROOT, "questions.md"),
        questions,
        f"DelucionQA -- {len(questions)} questions",
        preamble,
    )

    # Prefer questions whose reference answers were all judged adherent, so the
    # starter set is not seeded with examples the dataset itself flagged as
    # hallucinated.
    clean = [q for q in questions if q[1]["adherent"] == q[1]["total"]]
    write_questions(
        os.path.join(OUT_ROOT, "starter.md"),
        clean[:STARTER_COUNT],
        f"DelucionQA -- starter set ({min(STARTER_COUNT, len(clean))} questions)",
        "Questions whose reference answers were all judged fully supported by the source\n"
        "passages. Work through these first.\n\n" + preamble,
    )

    with open(os.path.join(OUT_ROOT, "README.md"), "w") as handle:
        handle.write(
            f"""# DelucionQA test corpus

Fetched by `scripts/fetch_delucionqa.py`. Not in git (see .gitignore) -- re-run
the script to restore.

- `corpus/` -- {len(passages)} deduplicated manual passages across {file_count} .txt files.
  Ingest this whole folder into one LEXIS group.
- `starter.md` -- {min(STARTER_COUNT, len(clean))} questions to try first.
- `questions.md` -- all {len(questions)} unique questions with reference answers.
- `raw/` -- untouched API rows, for the automated harness later.

Source: DelucionQA (Jeep 2023 Gladiator owner's manual) via the `delucionqa`
subset of https://huggingface.co/datasets/galileo-ai/ragbench

## What this corpus is not

`documents` in DelucionQA are the passages RETRIEVED for each question, not the
complete owner's manual. This corpus is therefore the union of gold contexts:
every question is answerable from it, with far fewer distractor sections than
the real manual. That makes it good for telling model errors apart from
retrieval errors -- the supporting text is definitely present -- but retrieval
will look easier here than it would at full-manual scale.
"""
        )

    print(f"\ncorpus:    {len(passages)} passages -> {file_count} files")
    print(f"questions: {len(questions)} unique ({len(clean)} fully-supported)")
    print(f"written to {OUT_ROOT}/")


if __name__ == "__main__":
    main()
