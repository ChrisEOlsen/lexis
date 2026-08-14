---
tags: [query-path, llm]
---

# Generation

**The final step: grounded answering.** Builds a "use only this context" prompt from the retrieved passages' actual text and asks the local model for the answer the user sees.

Source: `src/core/generation.c`.

- The prompt forbids guessing beyond the context, forbids meta-references ("the context", chunk numbers), and demands plain prose.
- Two entry points: `generation_generate_answer()` (CLI, single-turn) and `generation_generate_answer_with_history()` ([[Query Worker]] — answers the *original* question, not the search reformulation, with chat history windowed in on a token budget).
- Passage text is fetched fresh from [[Postgres Store]] by passage id.

**Called by:** [[CLI]], [[Query Worker]]. **Calls:** [[Local LLM Client]]. **This is where output leaves the pipeline.**
