#!/usr/bin/env bash
# Exports MS MARCO passage-ranking data for `lexis bulk-ingest`/`lexis eval`.
# Safe to re-run; CSV quoting required (passage text contains backslash/quotes).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS_OUT="$ROOT/data/corpus/corpus_csv.tsv"
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
