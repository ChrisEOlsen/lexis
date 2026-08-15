#!/usr/bin/env bash
# Exports any BeIR/* dataset into the exact formats `lexis bulk-ingest`
# and `lexis eval` consume, so LEXIS results line up against the BEIR
# benchmark's published BM25/dense-retriever nDCG@10 tables without
# running any baseline locally. Generalizes scripts/export_msmarco.sh's
# duckdb approach (BeIR repos share one layout: corpus/ + queries/
# parquet, and a BeIR/<name>-qrels sibling repo of TSVs).
#
# Usage: scripts/export_beir.sh <dataset> [qrels_split]
#   e.g.  scripts/export_beir.sh scifact test
#         scripts/export_beir.sh nfcorpus test
#
# Outputs under data/eval/beir/<dataset>/:
#   corpus_csv.tsv   <doc_id><TAB><title + text>, RFC4180 CSV-quoted, no
#                    header -- bulk-ingest input. Title is prepended:
#                    published BM25 baselines index title+text, and BEIR
#                    titles carry real signal (papers' titles especially).
#   qrels_<split>.tsv  the split verbatim (query-id/corpus-id/score,
#                    header) -- `lexis eval`'s second argument.
#   queries_<split>.tsv  <query_id><TAB><query_text>, no header, no
#                    quoting, restricted to queries in the split's qrels.
set -euo pipefail

DATASET="${1:?usage: scripts/export_beir.sh <dataset> [qrels_split]}"
SPLIT="${2:-test}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT/data/eval/beir/$DATASET"
CORPUS_OUT="$OUT_DIR/corpus_csv.tsv"
QRELS_OUT="$OUT_DIR/qrels_${SPLIT}.tsv"
QUERIES_OUT="$OUT_DIR/queries_${SPLIT}.tsv"

mkdir -p "$OUT_DIR"

if [ -f "$CORPUS_OUT" ]; then
    echo "Already have $(basename "$CORPUS_OUT"), skipping corpus export."
else
    echo "Exporting $DATASET corpus to $CORPUS_OUT ..."
    duckdb -c "
        INSTALL httpfs; LOAD httpfs;
        COPY (
            SELECT _id,
                   trim(concat_ws(' ', coalesce(title, ''), coalesce(text, ''))) AS text
            FROM 'hf://datasets/BeIR/$DATASET/corpus/*.parquet'
        )
        TO '$CORPUS_OUT' (FORMAT CSV, DELIMITER '\t', HEADER false);
    "
fi

if [ -f "$QRELS_OUT" ]; then
    echo "Already have $(basename "$QRELS_OUT"), skipping qrels download."
else
    echo "Fetching $SPLIT qrels to $QRELS_OUT ..."
    curl -fL -o "$QRELS_OUT" \
        "https://huggingface.co/datasets/BeIR/$DATASET-qrels/resolve/main/${SPLIT}.tsv"
fi

if [ -f "$QUERIES_OUT" ]; then
    echo "Already have $(basename "$QUERIES_OUT"), skipping queries export."
else
    echo "Exporting $SPLIT queries to $QUERIES_OUT ..."
    duckdb -c "
        INSTALL httpfs; LOAD httpfs;
        COPY (
            SELECT _id,
                   replace(replace(replace(text, chr(9), ' '), chr(13), ' '), chr(10), ' ') AS text
            FROM 'hf://datasets/BeIR/$DATASET/queries/*.parquet'
            WHERE _id IN (
                SELECT CAST(\"query-id\" AS VARCHAR)
                FROM read_csv('$QRELS_OUT', delim = '\t', header = true)
            )
        )
        TO '$QUERIES_OUT' (FORMAT CSV, DELIMITER '\t', HEADER false, QUOTE '');
    "
fi

echo "Done: $OUT_DIR"
