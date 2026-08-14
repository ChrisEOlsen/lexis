---
tags: [config]
---

# Config

**Centralized runtime settings** in `config/lexis.conf`, parsed one getter per key — deliberately not a general key-value system.

Source: `src/core/config.c`, `include/config.h`.

Two settings today:
- `mode = testing | production` — testing (the default when unset) turns on [[Query Log]]'s full pipeline observability; production skips it.
- `model_path` — the GGUF file [[Local LLM Client]] loads. Centralized 2026-08-13 after being hardcoded in five places that drifted every model swap. Missing file/line falls back quietly to `LEXIS_DEFAULT_MODEL_PATH` in config.h. `scripts/download_model.sh` reads this same line to decide what to fetch, so script and code can't diverge again.

**Read by:** [[CLI]], [[App Controller]], [[Eval Harness]], the depth_ab script.
