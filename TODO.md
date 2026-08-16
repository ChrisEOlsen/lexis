# TODO

Backlog of improvements that have a clear shape but no scheduled slot.
Measurement runs and their sequencing live in TESTING.md, not here.

## Speculative decoding for answer generation

The biggest remaining latency lever. Generation dominates every
answer's wall time and is memory-bandwidth-bound on Apple Silicon (the
GPU reads all ~5GB of gemma-4-E4B weights per generated token), so no
amount of "more GPU" helps -- but speculative decoding sidesteps the
bound: a tiny draft model proposes several tokens, the big model
verifies them in ONE weight-read pass. Typical gain 1.5-2x on
generation; would take the ~6.6s mean answer toward ~4s.

- llama.cpp supports it natively (draft-model API); the integration
  point is local_llm_client.c's generation loop.
- Needs a draft model from the same family/tokenizer as the target --
  for gemma-4-E4B, the smallest gemma-4 variant quantized small
  (~300-600MB) is the natural candidate. Tokenizers must match or
  llama.cpp rejects the pairing.
- Config-gate it like the reranker (`draft_model_path`, unset = off)
  so it is measurable in isolation.
- Measure with the starter set first (latency + coverage must hold --
  speculative decoding is exact, output should be bit-identical at
  greedy sampling, so any quality change indicates a bug), then a 913
  timing run.

While in that code: batch the reranker's per-passage embeddings
(currently one llama_encode call per candidate; packing sequences per
call would roughly halve its ~0.5-1s cost). Small win, same file
family, same measurement discipline.
