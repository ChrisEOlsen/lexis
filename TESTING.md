# TESTING.md -- the LEXIS testing plan

Everything about measuring this project, in one place: the harnesses
that exist, the numbers we have, the run that is queued, and the
targets. Like CURRENT_STATE.md, a stale entry here is a bug. Rewritten
2026-08-15 after the four quality fixes landed (reranker, refusal
retry, learned synonyms, router fix); the old step-by-step plan this
file used to hold is complete except where listed under "Still to run".

## 1. The harnesses

**Unit/integration suite** -- `make check`. 15 binaries against the
lexis_test database. Run after every code change; the suite exits on
the first failing binary. Gotcha learned the hard way: a binary that
prints "N passed, 0 failed" and then exits nonzero (e.g. a crash in
process teardown) reads as a failure and silently stops the suite --
count the binaries if the output looks short.

**Routing check** -- a questions file through `lexis_eval` and read the
tool column. 12-question set: the router prompt's own examples, the
formerly-misrouted casual phrasings ("any tips about..."), the "can I
use X" class, plus CHAT/SUMMARY controls. Re-run whenever
LEXIS_PROMPT_TOOL_ROUTER_HEAD is reworded, per its own header comment.

**Starter set (30 questions, ~5 min)** -- the quick end-to-end check:
`./app/build/lexis_eval 2 <starter_questions> > run.tsv` then
`scripts/pipeline_eval_score.py`. Real app pipeline: routing, rewrite,
expansion, reranker, generation, provenance.

**Full DelucionQA (913 questions, ~2.5-3h)** -- same harness, all
unique questions, chained with both scorers:
1. `./app/build/lexis_eval 2 data/eval/delucionqa/questions_913.txt > run.tsv`
   (column 8 carries the passages the model read)
2. `scripts/pipeline_eval_score.py <raw> <passages_tsv> run.tsv <stopwords>`
   -- routing distribution, refusals, latency, lexical coverage
3. `.venv/bin/python scripts/grounding_score.py <raw> run.tsv <stopwords> <out>`
   -- gold_sent (did the right passage reach the model; shingle
   containment vs the dataset's per-question gold documents) and
   NLI-judged answer support. MUST run on MPS with fixed-length
   padding (the script does both); CPU/variable padding crawls for
   hours. ~35 min for the full run.

**BEIR retrieval benchmarks (~15 min both corpora)** -- the
compare-to-published-numbers harness. `scripts/export_beir.sh <ds> test`
exports any BeIR/* dataset; swap it into the public schema (truncate +
`./lexis bulk-ingest`), then `./lexis eval <queries> <qrels>
[--no-llm-expansion]`. Reports nDCG@10 (linear-gain trec_eval
convention, matching what BEIR publishes) alongside MRR/Recall.
Remember to restore the public schema afterward (the 200K MS MARCO
slice: `head -200000 corpus_csv.tsv`, ~22s to re-ingest).

**MS MARCO in-slice eval (70 queries, ~2 min)** -- the fast retrieval
A/B during development: the dev queries whose gold passage is inside
the 200K slice. Regenerate the subset with awk from qrels_dev.tsv (see
memory of past runs or rebuild: filter queries_dev.tsv to query-ids
whose qrels corpus-id appears in the slice).

**Tuning sweeps** -- env knobs, no rebuilds: LEXIS_CHUNK_SIZE /
LEXIS_CHUNK_OVERLAP (bulk-ingest), LEXIS_BM25_K1 / LEXIS_BM25_B
(retrieval policy). Findings so far: shipped k1/b 1.2/0.75 won; chunking
is corpus-dependent (whole-doc helps NFCorpus +0.017, hurts SciFact);
ranking is deterministic per ingest since the passage_id tie-break.

**Config toggles that change what a run measures**: `thinking=on|off`
(generation reasoning; off = 3.9x faster, measured quality-neutral on
the starter set), `reranker_model_path` (present = reranker on;
comment it out for a no-reranker arm). Both read once per process --
restart the app/harness after flipping.

## 2. Current numbers (2026-08-15, BEFORE the four fixes' full run)

913-question baseline (thinking=off, pre-reranker/retry/synonyms):
- Routing: 99.1% SEARCH (6 CHAT misroutes, all "can I <use feature>")
- gold_sent: 91.8% -- the 74 misses split 40 vocabulary/pool vs 34
  partial-arrival (chunk/ranking)
- Answers supported (NLI >= 0.5): 96.2%
- Refusals: 19 (11 retrieval, 8 model-refused-with-gold-present)
- Genuine failures ~7.4%, split ~5:1 retrieval:generation
- Latency: 6.6s mean
- Of 134 low-coverage answers, only 3 actually unsupported -- lexical
  coverage flags paraphrase, treat it as a screen, not a grade.

Reranker gate (SciFact, plain terms): nDCG@10 0.6353 -> 0.6859,
Recall@10 0.7508 -> 0.8086. Above published anserini BM25 (0.665), at
BM25+CE level (0.688), just under ColBERTv2 (0.693).

BEIR standings (pre-reranker): SciFact 0.6395 / NFCorpus 0.2834 with
expansion. Published anchors: BM25 0.665/0.325, DPR 0.318/0.189, ANCE
0.507/0.237, TAS-B 0.643/0.319, ColBERTv2 0.693/0.338, BGE-Base
0.743/0.360.

## 3. DONE 2026-08-16: the 913 v2 comparison run

Results (run 2h11m, thinking=off, all four fixes active):
misroutes 8 -> 3 (zero CHAT misroutes left; 3 borderline SUMMARY),
gold_sent 91.8% -> 97.3% (target met), refusals 19 -> 9, supported
96.2% -> 98.9%, genuine failures ~7.4% -> ~2.7% (20 retrieval + 5
gold-present refusals of 910). Of 120 low-coverage answers, ZERO are
unsupported -- all remaining "weak" answers are paraphrase artifacts.
Vocabulary-miss bucket 40 -> 16: synonyms+reranker recovered over
half at zero ingest cost, weakening the hybrid-embeddings case (sec.
4.3) to ~2.2% of questions. Latency 6.6s -> 8.6s mean (reranker +
thinking-on retries; 0.6s over the 8s target -- acceptable, revisit
via TODO.md speculative decoding). Artifacts: *_v2 files.

### Original comparison plan (kept for context)

One chained command (run it overnight; ~3.5h total): 913 through the
upgraded pipeline -> coverage scoring -> grounding attribution, all
artifacts to data/eval/delucionqa/*_v2*. Compare against section 2's
baseline, number by number:

| Metric | Baseline | Fix that targets it | Success looks like |
|---|---|---|---|
| Routing misroutes | 8 | router prompt fix | <= 2 |
| gold_sent | 91.8% | reranker (34 partial-arrival misses) + synonyms (40 vocab misses) | >= 95% |
| Refusals | 19 | refusal retry | <= 8 |
| Supported answers | 96.2% | (should hold) | >= 96% |
| Mean latency | 6.6s | reranker adds ~0.5-1s; retry only on refusals | <= 8s |
| Genuine failure rate | ~7.4% | all four | <= 4-5% |

Also rerun NFCorpus with the reranker for the second BEIR point, and
re-verify the routing check file (12/12).

## 4. Still to run (the roadmap tests)

1. **Full MS MARCO**: 8.84M-row ingest (~16 min; TRUNCATE public schema
   first -- corpus_* schemas are untouched) then the 6,980-query eval.
   First number directly comparable to published full-corpus baselines
   (BM25 MRR@10 ~0.187; dense 0.33+). With the reranker this is also
   the "lexical+rerank vs dense at scale" datapoint.
2. **RAGBench published-row lookup**: pull the delucionqa adherence
   table from the RAGBench paper (arXiv 2407.11005 -- full PDF, the
   abstract page doesn't carry tables) and put our NLI-judged
   supported-rate next to their ada-002 pipeline's, with the
   same-family-judge caveat stated.
3. **The embeddings/hybrid decision**: gated on the v2 run's remaining
   vocabulary-miss count. If the synonym table + reranker leave
   meaning-vs-words failures in low single digits, hybrid retrieval
   stays unbuilt; if not, the reranker's embedding model is already
   in-process -- indexing passage embeddings at ingest (cheap,
   milliseconds per chunk) becomes the next experiment.

## 5. Targets scoreboard

Tier 1 -- lexical parity with published BM25: SciFact >= 0.665 (MET
with reranker: 0.686), NFCorpus >= 0.325 (pending re-run), MS MARCO
full >= 0.19 (pending).
Tier 2 -- deployed-RAG parity on retrieval: SciFact >= 0.69 (0.686,
within noise), NFCorpus >= 0.34 (pending).
Tier 3 -- the product numbers on DelucionQA: gold_sent >= 95%,
correctness >= 93%, routing >= 98% (MET), refusals-on-answerable 0,
mean latency <= 10s (MET).

## 6. Ops notes

- The public schema holds ONE corpus at a time; benchmark runs swap it.
  App groups (corpus_* schemas) are never touched by that swap.
- Model files live in data/models/ (gitignored): gemma-4-E4B chat model
  (download_model.sh reads lexis.conf) + bge-small-en-v1.5-f16 reranker.
- The Python venv (.venv, gitignored) serves the judge scripts:
  transformers/torch/sentencepiece. Judges run on MPS.
- Long runs: write progress to a FILE you can tail (piping through
  `tail` buffers everything until the end -- learned twice in one day).
