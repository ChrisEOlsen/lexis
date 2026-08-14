---
tags: [query-path, llm]
---

# Tool Router

**The per-message decision: which of three tools handles this prompt?** A single one-shot LLM classification — deliberately not a multi-step tool loop, and no tool takes a model-supplied argument.

Source: `src/core/tool_router.c`, `include/tool_router.h`.

The three choices:
- `TOOL_SEARCH_PASSAGES` — specific, lookup-style questions → the BM25 pipeline.
- `TOOL_SUMMARIZE_CORPUS` — broad whole-collection questions → [[Corpus Summary]]. (Replaced an older READ tool that re-fed whole documents through the context window per request — slowest thing in the app, and it overlapped SUMMARY so heavily that three sharp choices route more reliably than four muddy ones.)
- `TOOL_CONVERSE` — greetings, thanks, questions about the conversation itself. Exists because both retrieval tools produce nonsense for conversational input.

- One small prompt (~500 reserved tokens), answered with one word, prefilled to skip reasoning — a classification doesn't need chain-of-thought.
- Windows chat history into the prompt on a token budget (its own copy of the windowing walk — each call site's budget differs).
- Measured 96.7% on a 30-question routing test when gemma-4-E2B was adopted.

**Called by:** [[Query Worker]]. **Calls:** [[Local LLM Client]].
