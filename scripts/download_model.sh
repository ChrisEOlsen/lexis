#!/usr/bin/env bash
# Downloads the local GGUF model LEXIS runs query formulation and
# generation against (see local_llm_client.c). Not committed to git --
# it's a ~1.9GB binary -- so anyone setting up the repo fresh runs this
# once. Safe to re-run; skips the download if the file already exists.
set -euo pipefail

MODEL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/data/models"
MODEL_FILE="Llama-3.2-3B-Instruct-Q4_K_M.gguf"
MODEL_URL="https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/${MODEL_FILE}"

mkdir -p "$MODEL_DIR"

if [ -f "$MODEL_DIR/$MODEL_FILE" ]; then
    echo "Already have $MODEL_FILE, skipping download."
    exit 0
fi

echo "Downloading $MODEL_FILE (~1.9GB) to $MODEL_DIR ..."
curl -L -o "$MODEL_DIR/$MODEL_FILE" "$MODEL_URL"
echo "Done."
