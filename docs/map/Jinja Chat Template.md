---
tags: [llm]
---

# Jinja Chat Template

**The one C++ translation unit in the core** — real Jinja2 rendering (vendored minja + nlohmann-json) for chat templates too sophisticated for llama.cpp's built-in template matcher.

Source: `src/core/jinja_chat_template.cpp` (C++17; everything else in the core is C11).

- Exists because gemma-4's chat template is genuine Jinja2; its template also defaults `enable_thinking` to false on its own, which removed the prefill hack the Qwen era needed.
- Compiled separately by the Makefile (`$(CXX)`), linked into every binary; the reason `-lc++` appears at link time.

**Used by:** [[Local LLM Client]].
