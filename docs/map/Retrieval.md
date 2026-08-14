---
tags: [query-path]
---

# Retrieval

**The shared orchestrator — THE pipeline, defined once.** `retrieval_run()` takes a question (plus the app's rewritten question when there is one) and returns ranked passages *and* every stage's artifacts: the expansion prompt, the model's raw reply, the final term list with its originals/expansions boundary, timings.

Source: `src/core/retrieval.c`, `include/retrieval.h`.

Two rules keep it single-sourced:
1. **Caller differences are `RetrievalPolicy` values, never code branches.** There is no "cli" flag and there must never be one. [[Eval Harness]]'s deeper, untrimmed search is a visible policy override at its call site; [[CLI]] and [[Query Worker]] share `retrieval_default_policy()`.
2. **Observers read artifacts from `RetrievalRun`, they don't re-run stages.** [[Query Log]] and the app's source inspector consume the same record of the one run that happened.

The sequence inside: terms union → sense-filtered expansion ([[Query Formulation]] primitives, degrades to plain terms on any failure) → weighted [[BM25 Search]] with coordination → trim.

What stays outside, deliberately: [[Tool Router]], chat history and contextualization (the rewritten question is an *input*), corpus scoping, [[Generation]], presentation.

**Called by:** [[CLI]], [[Query Worker]], [[Eval Harness]] — a pipeline change lands in all three at once.
