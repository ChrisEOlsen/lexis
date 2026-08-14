#!/usr/bin/env bash
# Exports the MS MARCO passage-ranking data LEXIS consumes, capturing the
# previously ad-hoc duckdb corpus export flagged as a known gap in
# CURRENT_STATE.md ("Corpus export isn't scripted"). Safe to re-run; each
# output is skipped if it already exists on disk.
#
# Outputs:
#
#   corpus_csv.tsv (repo root)
#       <pid><TAB><text>, RFC4180 CSV-quoted, no header -- the input
#       `lexis bulk-ingest` expects. Plain TSV is NOT safe here: real
#       MS MARCO passage text contains literal backslash and double-quote
#       characters, and Postgres COPY's TEXT format treats backslash as
#       its escape character; FORMAT CSV is what makes Phase 1's COPY
#       safe (see CURRENT_STATE.md, "Ingestion").
#
#   data/eval/msmarco/qrels_dev.tsv
#       BeIR/msmarco-qrels dev split (query-id<TAB>corpus-id<TAB>score,
#       with header) -- `lexis eval`'s second argument, fetched verbatim.
#
#   data/eval/msmarco/queries_dev.tsv
#       <query_id><TAB><query_text>, no header, no CSV quoting (eval.c
#       splits each line on the first tab; it does not CSV-parse),
#       restricted to the queries that appear in the dev qrels -- `lexis
#       eval`'s first argument. Tabs/newlines inside query text (none
#       expected, but cheap to guarantee) are flattened to spaces to
#       keep the one-row-per-line invariant.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS_OUT="$ROOT/corpus_csv.tsv"
EVAL_DIR="$ROOT/data/eval/msmarco"
QRELS_OUT="$EVAL_DIR/qrels_dev.tsv"
QUERIES_OUT="$EVAL_DIR/queries_dev.tsv"

mkdir -p "$EVAL_DIR"

if [ -f "$CORPUS_OUT" ]; then
    echo "Already have $(basename "$CORPUS_OUT"), skipping corpus export."
else
    echo "Exporting corpus (8.84M passages, ~3GB on disk) to $CORPUS_OUT ..."
    duckdb -c "
        INSTALL httpfs; LOAD httpfs;
        COPY (SELECT _id, text FROM 'hf://datasets/BeIR/msmarco/corpus/*.parquet')
        TO '$CORPUS_OUT' (FORMAT CSV, DELIMITER '\t', HEADER false);
    "
fi

if [ -f "$QRELS_OUT" ]; then
    echo "Already have $(basename "$QRELS_OUT"), skipping qrels download."
else
    echo "Fetching dev qrels to $QRELS_OUT ..."
    curl -fL -o "$QRELS_OUT" \
        "https://huggingface.co/datasets/BeIR/msmarco-qrels/resolve/main/dev.tsv"
fi

if [ -f "$QUERIES_OUT" ]; then
    echo "Already have $(basename "$QUERIES_OUT"), skipping queries export."
else
    echo "Exporting dev queries to $QUERIES_OUT ..."
    duckdb -c "
        INSTALL httpfs; LOAD httpfs;
        COPY (
            SELECT _id,
                   replace(replace(replace(text, chr(9), ' '), chr(13), ' '), chr(10), ' ') AS text
            FROM 'hf://datasets/BeIR/msmarco/queries/*.parquet'
            WHERE _id IN (
                SELECT CAST(\"query-id\" AS VARCHAR)
                FROM read_csv('$QRELS_OUT', delim = '\t', header = true)
            )
        )
        TO '$QUERIES_OUT' (FORMAT CSV, DELIMITER '\t', HEADER false, QUOTE '');
    "
fi

echo "Done."
