# Evaluation

Every change to retrieval or answering gets measured before it stays.
Two kinds of measurement:

## Retrieval quality (no answers, just search)

`./lexis eval <queries.tsv> <qrels.tsv>` scores search against labeled
relevance data and reports nDCG@10, MRR@10, and Recall -- the metrics
the information-retrieval literature publishes, so LEXIS numbers can
sit next to published ones directly.

Current standing on BEIR (the standard zero-shot retrieval benchmark),
nDCG@10 on SciFact:

| System | Score |
|---|---|
| DPR (embedding retriever, 2020) | 0.318 |
| ANCE (embedding retriever, 2021) | 0.507 |
| Published BM25 (anserini) | 0.665 |
| **LEXIS (BM25 + expansion + reranker)** | **0.686** |
| ColBERTv2 | 0.693 |
| BGE-Base (modern embedder) | 0.743 |

LEXIS scores above the classic lexical baseline and above the
embedding retrievers most deployed RAG systems were built on, using no
document embeddings at all.

## End-to-end quality (real questions, real answers)

The app pipeline is tested against DelucionQA: 913 questions about a
900-page car manual, with human-annotated reference answers. Latest
full run:

- The right passage reached the model for **97.3%** of questions.
- **98.9%** of answers were faithful to the passages given (checked
  by an independent judge model, not self-graded).
- 9 refusals out of 910 answerable questions.
- Routing picked the right tool 99.7% of the time.
- 8.6 seconds per answer on an M-series Mac, fully local.
- Estimated genuine failure rate: about 3%.

## How the judging works

Lexical overlap with reference answers is only a screening signal (it
punishes correct answers that use different words). Two deeper checks
run on full-set evaluations:

- **Gold-passage tracking** -- the test data records which manual
  passages answer each question; we check whether any of them actually
  reached the model. This separates "search failed" from "the model
  failed" per question.
- **Support judging** -- a small NLI model checks each answer against
  the passages it was given: supported or not. This catches making
  things up, independent of any reference answer.

The measurement scripts live in `scripts/` and the internal runbooks
with full history live outside the repo.
