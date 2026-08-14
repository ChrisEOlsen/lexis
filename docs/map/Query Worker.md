---
tags: [app, query-path]
---

# Query Worker

**One chat prompt, end to end, off the UI thread.** The heart of the app's prompt→output flow.

Source: `app/src/QueryWorker.{h,cpp}`.

Sequence for every user message:
1. Load the session's message history; persist the user's question (`public.chat_sessions`/`chat_messages` via [[Postgres Store]]).
2. [[Tool Router]] — one LLM call decides SEARCH / SUMMARY / CHAT.
3. **SEARCH**: `query_formulation_contextualize_question()` resolves pronouns/follow-ups against history into a standalone query ([[Query Formulation]]), then searches the **union** of the raw and reformulated questions' terms — deliberately *not* the WordNet+LLM expansion variant. → [[BM25 Search]] with a candidate ceiling, then `bm25_result_set_trim()` cuts to a token budget. → [[Generation]] answers the *original* question (reformulation only ever helped retrieval) with history awareness.
4. **SUMMARY**: [[Corpus Summary]] answers whole-collection questions from a cached overview; skips retrieval entirely.
5. **CHAT**: answer straight from [[Local LLM Client]] — greetings/meta-questions produce nonsense through either retrieval tool.
6. Persist the assistant's answer; fetch document names/chunk ids for the UI's source citations.

Opens its own corpus-scoped [[Postgres Store]] connection (`pg_store_use_corpus`).
