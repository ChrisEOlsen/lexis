---
tags: [llm]
---

# Local LLM Client

**The single in-process LLM every call site shares.** Loads one GGUF model via llama.cpp's embedded C API — no Ollama server, no HTTP, no per-query API cost. Metal GPU offload on Apple Silicon.

Source: `src/core/local_llm_client.c`.

- **One global context, no concurrency support** — callers must serialize ([[App Controller]] enforces this in the app; the CLI is single-threaded through it anyway).
- Model path comes from [[Config]] (`model_path` in lexis.conf) — currently `gemma-4-E4B-it-Q4_K_M.gguf` (5.0GB, Q4_K_M). Model history: Llama-3.2-3B → Qwen3.5-4B (unprompted thinking, reverted) → Qwen3.5-2B (thinking suppressed via prefill) → gemma-4-E2B (8GB machine) → E4B (24GB M5 machine).
- Chat templating goes through [[Jinja Chat Template]] because Gemma's template is real Jinja2, beyond `llama_chat_apply_template()`'s built-in matcher.
- Load cost ~9–19s, which is why every long-lived caller loads once up front ([[Model Loader]] in the app, one init per [[CLI]]/[[Eval Harness]] process).

The five call sites: [[Query Formulation]] (both variants), [[Tool Router]], [[Corpus Summary]], [[Generation]] (grounded + conversational).
