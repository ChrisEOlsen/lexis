---
tags: [app, ingestion]
---

# Ingest Worker

**The app's ingestion thread**: a file dropped on a group becomes passages in that group's index, off the UI thread.

Source: `app/src/IngestWorker.{h,cpp}`.

- Routes each file through [[Document Extractors]] by format (PDF, DOCX, image-OCR, plain text), then [[Ingest Primitives]] → [[Postgres Store]], scoped to the group's schema.
- Opens its own connection; reports progress back to [[App Controller]] for the UI.
- Never calls the LLM — that keeps [[Local LLM Client]]'s serialization contract simple (see [[Corpus Summary]] for the one design decision this forced).

**Spawned by:** [[App Controller]].
