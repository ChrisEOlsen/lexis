#!/usr/bin/env bash
# Downloads the GGUF model in config/lexis.conf's model_path (fallback: see include/config.h).
# Safe to re-run; fetches from unsloth's GGUF mirrors by filename derivation.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_MODEL_PATH="data/models/gemma-4-E4B-it-Q4_K_M.gguf"

# Last (non-comment) model_path line wins, matching config.c's parser.
MODEL_PATH="$(sed -n 's/^[[:space:]]*model_path[[:space:]]*=[[:space:]]*//p' \
    "$ROOT/config/lexis.conf" 2>/dev/null | tail -1)"
MODEL_PATH="${MODEL_PATH:-$DEFAULT_MODEL_PATH}"

MODEL_FILE="$(basename "$MODEL_PATH")"
# Strip quant suffix to recover the model name unsloth's "-GGUF" repos use.
MODEL_NAME="$(echo "$MODEL_FILE" | sed -E 's/-(UD-)?(I?Q[0-9][A-Za-z0-9_]*)\.gguf$//')"
if [ "$MODEL_NAME" = "$MODEL_FILE" ]; then
    echo "Could not derive a HuggingFace repo from '$MODEL_FILE' (unexpected" >&2
    echo "filename shape). Download it manually into data/models/ instead." >&2
    exit 1
fi
MODEL_URL="https://huggingface.co/unsloth/${MODEL_NAME}-GGUF/resolve/main/${MODEL_FILE}"

mkdir -p "$ROOT/$(dirname "$MODEL_PATH")"

if [ -f "$ROOT/$MODEL_PATH" ]; then
    echo "Already have $MODEL_FILE, skipping download."
    exit 0
fi

echo "Downloading $MODEL_FILE to $ROOT/$(dirname "$MODEL_PATH") ..."
curl -fL -o "$ROOT/$MODEL_PATH" "$MODEL_URL"
echo "Done."
