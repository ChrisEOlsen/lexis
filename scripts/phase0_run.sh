#!/usr/bin/env bash
# Phase 0: measure where BM25 ranks the correct passage across DelucionQA.
# No language model is loaded -- see scripts/phase0_retrieval.c.
#
# usage: scripts/phase0_run.sh <corpus_id>
set -euo pipefail

CORPUS_ID="${1:?usage: scripts/phase0_run.sh <corpus_id>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

EVAL_DIR="data/eval/delucionqa"
WORK="$EVAL_DIR/phase0"
mkdir -p "$WORK"

PG_CONFIG=/opt/homebrew/opt/postgresql@18/bin/pg_config
PG_INC="$($PG_CONFIG --includedir)"
PG_LIB="$($PG_CONFIG --libdir)"
LLAMA_DIR=/opt/homebrew/Cellar/llama.cpp/10180
GGML_DIR=/opt/homebrew/Cellar/ggml/0.18.0
# Connection string comes from the untracked config file (embeds the
# password -- see config/lexis.conf.example).
CONNINFO="$(sed -n 's/^db_conninfo[[:space:]]*=[[:space:]]*//p' config/lexis.conf | tail -1)"
[ -n "$CONNINFO" ] || { echo "set db_conninfo in config/lexis.conf" >&2; exit 1; }

echo "1/4 extracting unique questions"
python3 - "$EVAL_DIR/raw" "$WORK/questions.txt" <<'PY'
import json, os, sys
raw_dir, out = sys.argv[1], sys.argv[2]
seen, ordered = set(), []
for name in sorted(os.listdir(raw_dir)):
    if not name.endswith(".json"):
        continue
    for row in json.load(open(os.path.join(raw_dir, name))):
        q = " ".join((row.get("question") or "").split())
        if q and q not in seen:
            seen.add(q)
            ordered.append(q)
with open(out, "w") as fh:
    fh.write("\n".join(ordered) + "\n")
print(f"    {len(ordered)} unique questions")
PY

echo "2/4 dumping ingested passages"
/opt/homebrew/opt/postgresql@18/bin/psql "$CONNINFO" -tAF$'\t' \
  -c "set search_path to corpus_${CORPUS_ID}, public;
      select id, document_name, chunk_id, replace(replace(text, chr(9), ' '), chr(10), ' ') from passages;" \
  > "$WORK/passages.tsv"
echo "    $(wc -l < "$WORK/passages.tsv") passages"

echo "3/4 building + running retrieval (no model loaded)"
# jinja_chat_template.o comes from the normal `make` build; the core sources
# link it in even though this harness never reaches the model path.
clang -std=c11 -O2 -Iinclude -Iinclude/vendor -I"$PG_INC" \
  -I"$LLAMA_DIR/include" -I"$GGML_DIR/include" \
  -o "$WORK/phase0_retrieval" scripts/phase0_retrieval.c \
  $(grep '^CORE_SRCS' Makefile | sed 's/^CORE_SRCS := //') build/jinja_chat_template.o \
  -L"$PG_LIB" -lpq -lm -lpthread \
  -L"$LLAMA_DIR/lib" -L"$GGML_DIR/lib" -lllama -lggml -lggml-base \
  -Wl,-rpath,"$LLAMA_DIR/lib" -Wl,-rpath,"$GGML_DIR/lib" -lc++

"$WORK/phase0_retrieval" "$CORPUS_ID" "$WORK/questions.txt" > "$WORK/retrieval.tsv"

echo "4/4 scoring"
python3 scripts/phase0_score.py "$EVAL_DIR/raw" "$WORK/passages.tsv" "$WORK/retrieval.tsv"
