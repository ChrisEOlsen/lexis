#!/usr/bin/env bash
# Downloads the local GGUF model LEXIS runs query formulation and
# generation against (see local_llm_client.c). Not committed to git --
# it's a multi-GB binary -- so anyone setting up the repo fresh runs
# this once. Safe to re-run; skips the download if the file already
# exists.
#
# Which model: read from config/lexis.conf's `model_path` line, so this
# script can't drift from what the code actually loads (it did once --
# the code moved from Llama-3.2-3B to gemma-4-E2B while this script kept
# fetching Llama). Falls back to the same default as include/config.h's
# LEXIS_DEFAULT_MODEL_PATH; keep the two in sync when the default model
# changes.
#
# Download source: unsloth's GGUF mirrors, whose repo naming is uniform
# enough to derive from the filename ("<model>-<quant>.gguf" lives in
# "unsloth/<model>-GGUF"). A model hosted elsewhere means updating this
# derivation -- the error below says so rather than guessing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_MODEL_PATH="data/models/gemma-4-E4B-it-Q4_K_M.gguf"

# Last (non-comment) model_path line wins, matching config.c's parser.
MODEL_PATH="$(sed -n 's/^[[:space:]]*model_path[[:space:]]*=[[:space:]]*//p' \
    "$ROOT/config/lexis.conf" 2>/dev/null | tail -1)"
MODEL_PATH="${MODEL_PATH:-$DEFAULT_MODEL_PATH}"

MODEL_FILE="$(basename "$MODEL_PATH")"
# Strip the quantization suffix (-Q4_K_M.gguf, -Q6_K.gguf, -UD-Q4_K_XL.gguf,
# ...) to recover the model name unsloth's "-GGUF" repos are named after.
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
