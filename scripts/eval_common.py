"""Shared eval-scoring helpers: shingles, content words, refusal detection."""

import re

SHINGLE = 8

REFUSAL_MARKERS = (
    "don't have enough",
    "do not have enough",
    "not enough information",
    "no matching passages",
    "does not contain",
    "doesn't contain",
    "cannot answer",
    "can't answer",
    "please provide",
    "could you rephrase",
    "i only have",
    "i don't have access",
)


def normalize(text):
    return " ".join(text.lower().split())


def shingles(text):
    words = normalize(text).split()
    if len(words) < SHINGLE:
        return {" ".join(words)} if words else set()
    return {" ".join(words[i : i + SHINGLE]) for i in range(len(words) - SHINGLE + 1)}


def content_words(text, stopwords):
    words = re.findall(r"[a-z0-9]+", text.lower())
    return {w for w in words if w not in stopwords and len(w) > 2}


def looks_like_refusal(answer):
    low = answer.lower()
    return any(m in low for m in REFUSAL_MARKERS)
