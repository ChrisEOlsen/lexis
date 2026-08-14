---
tags: [app]
---

# App Controller

**The app's orchestrator**: the one QObject QML talks to. Owns the models behind every list view, spawns the worker threads, and serializes access to the single global LLM context.

Source: `app/src/AppController.{h,cpp}` (plus `CorpusListModel`, `DocumentListModel`, `ChatMessageListModel`, `ChatSessionListModel`).

- On startup: connects to Postgres, loads stopwords/WordNet/lemmatizer, reads the model path from [[Config]], and kicks off [[Model Loader]] immediately so the ~9–19s model load overlaps with whatever the user does first.
- A chat prompt becomes a [[Query Worker]]; a dropped file becomes an [[Ingest Worker]].
- **Serialization contract**: never runs a Query Worker concurrently with a Model Loader or another Query Worker — [[Local LLM Client]] has one global context and no concurrency support.

**Next in the prompt flow:** [[Query Worker]].
