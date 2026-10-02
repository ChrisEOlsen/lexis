#!/usr/bin/env python3
"""Builds data/synonyms/learned_neighbors.tsv from fastText nearest neighbors.
Usage: build_synonym_table.py [vec_path] (downloads ~680MB vectors if omitted)."""

import os
import sys
import urllib.request
import zipfile

import numpy as np

VOCAB = 50000
TOP_K = 8
MIN_COSINE = 0.55
MIN_LEN = 3
BLOCK = 2048
URL = "https://dl.fbaipublicfiles.com/fasttext/vectors-english/wiki-news-300d-1M.vec.zip"
OUT = os.path.join("data", "synonyms", "learned_neighbors.tsv")


def near_duplicate(a, b):
    """Inflections/variants the lemmatizer already handles -- not synonyms."""
    shorter, longer = (a, b) if len(a) <= len(b) else (b, a)
    return longer.startswith(shorter[: max(4, len(shorter) - 1)])


def main():
    if len(sys.argv) > 1:
        vec_path = sys.argv[1]
    else:
        scratch = os.environ.get("TMPDIR", "/tmp")
        zip_path = os.path.join(scratch, "wiki-news-300d-1M.vec.zip")
        vec_path = os.path.join(scratch, "wiki-news-300d-1M.vec")
        if not os.path.exists(vec_path):
            if not os.path.exists(zip_path):
                print(f"downloading {URL}", file=sys.stderr)
                urllib.request.urlretrieve(URL, zip_path)
            print("unzipping", file=sys.stderr)
            with zipfile.ZipFile(zip_path) as zf:
                zf.extract("wiki-news-300d-1M.vec", scratch)

    words, vecs = [], []
    with open(vec_path, encoding="utf-8") as fh:
        fh.readline()  # header: count dim
        for line in fh:
            parts = line.rstrip().split(" ")
            word = parts[0]
            if not word.isalpha() or not word.islower() or len(word) < MIN_LEN:
                continue
            words.append(word)
            vecs.append(np.asarray(parts[1:], dtype=np.float32))
            if len(words) >= VOCAB:
                break

    matrix = np.vstack(vecs)
    matrix /= np.linalg.norm(matrix, axis=1, keepdims=True)
    print(f"{len(words)} words loaded", file=sys.stderr)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    kept_rows = 0
    with open(OUT, "w") as out:
        for start in range(0, len(words), BLOCK):
            sims = matrix[start : start + BLOCK] @ matrix.T  # (block, vocab)
            for local, row in enumerate(sims):
                i = start + local
                word = words[i]
                row[i] = -1.0  # self
                neighbors = []
                for j in np.argsort(-row):
                    if row[j] < MIN_COSINE:
                        break
                    if near_duplicate(word, words[j]):
                        continue
                    neighbors.append(words[j])
                    if len(neighbors) >= TOP_K:
                        break
                if neighbors:
                    out.write(f"{word}\t{' '.join(neighbors)}\n")
                    kept_rows += 1
            print(f"  {min(start + BLOCK, len(words))}/{len(words)}", file=sys.stderr, flush=True)

    print(f"wrote {kept_rows} rows to {OUT}", file=sys.stderr)


if __name__ == "__main__":
    main()
