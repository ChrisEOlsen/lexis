"""
Unix socket server entrypoint for the Python sidecar (spec 5.2.2, 6, build
order Stage 6). Currently has nothing to dispatch to: POS tagging is
deferred (see pos_tagging.py, LIMITATIONS.md) and synonym expansion is
implemented natively in C (src/core/wordnet.c), not here. The sidecar's
remaining planned role is the optional cross-encoder reranker (spec 5.2.6,
Stage 9) -- this file is real scaffolding for that, not dead code, just not
yet needed.
"""
