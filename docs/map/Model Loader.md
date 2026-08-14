---
tags: [app, llm]
---

# Model Loader

**A QThread that pays the model-load cost early**: kicked off the moment [[App Controller]] constructs, so the ~9–19s GGUF load overlaps with whatever the user does first instead of stalling their first question.

Source: `app/src/ModelLoader.{h,cpp}`.

- Receives the model path (from [[Config]] via App Controller) and runs `local_llm_client_init()` on it.
- Signals readiness back; the chat UI gates on `modelReady`.
- Part of the serialization contract: App Controller never runs it concurrently with a [[Query Worker]] because [[Local LLM Client]]'s context is global.
