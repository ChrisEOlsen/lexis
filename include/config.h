/*
 * Centralized runtime settings (config/lexis.conf). Lets pipeline behavior
 * be toggled from one place rather than scattered per-feature flags --
 * `mode` gates query_log.c's pipeline observability logging (a real, if
 * small, per-query cost production traffic shouldn't pay), and
 * `model_path` names the local GGUF model every binary loads (previously
 * hardcoded separately in the CLI, the app, the eval harness, and the
 * depth_ab script, which drifted every model swap). See LIMITATIONS.md
 * for what config/lexis.conf.example still doesn't wire up (chunk size
 * etc.) -- this module deliberately parses exactly the settings something
 * needs today, one getter per key, not a general key-value config system.
 */

#ifndef LEXIS_CONFIG_H
#define LEXIS_CONFIG_H

/* Testing: query_log.c's full pipeline observability logging is active on
 * every query. Production: logging is skipped entirely -- no queries/
 * query_formulation_runs/search_runs/search_results/generation_runs
 * writes, avoiding their latency cost. */
typedef enum {
    LEXIS_MODE_TESTING,
    LEXIS_MODE_PRODUCTION
} LexisMode;

/* Reads a "mode = testing|production" line from the config file at `path`
 * (see config/lexis.conf.example for the format). A missing config file is
 * a normal, expected state (not every checkout has copied the example into
 * a real config/lexis.conf yet) and quietly defaults to LEXIS_MODE_TESTING
 * -- preserving today's always-on logging behavior. Production is an
 * explicit opt-in via the config file, never a silent default. */
LexisMode config_load_mode(const char *path);

/* Fallback when the config file is missing or has no model_path line.
 * Settled on Gemma-4-E4B after, in order: Llama-3.2-3B (no tool-routing
 * support in mind at the time) -> Qwen3.5-4B (reverted -- a genuine
 * "thinking" model, unprompted <think>...</think> before every answer,
 * real latency cost) -> Qwen3.5-2B (thinking suppressed via a prefill
 * hack; measured 86.7% on a 30-question SEARCH/READ tool-routing test)
 * -> Gemma-4-E2B (native tool-calling model; its chat template is real
 * Jinja2, too sophisticated for llama_chat_apply_template()'s built-in
 * matcher, which is why src/core/jinja_chat_template.cpp/minja exist at
 * all; no prefill hack needed; measured 96.7% on the identical
 * 30-question test) -> Gemma-4-E4B (same family one size up, adopted
 * when the 8GB-RAM machine that forced E2B was replaced by a 24GB one).
 * Keep scripts/download_model.sh's fallback in sync when this changes. */
#define LEXIS_DEFAULT_MODEL_PATH "data/models/gemma-4-E4B-it-Q4_K_M.gguf"

/* Reads the "model_path = <path to .gguf>" line from the config file at
 * `path`, falling back to LEXIS_DEFAULT_MODEL_PATH when the file or the
 * line is missing -- same quiet-fallback philosophy as config_load_mode.
 * Returns a malloc'd string the caller owns (free() it), or NULL only on
 * allocation failure. */
char *config_load_model_path(const char *path);

#endif /* LEXIS_CONFIG_H */
