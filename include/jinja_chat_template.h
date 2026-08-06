/*
 * C-callable bridge to a real Jinja2 chat-template renderer (minja,
 * vendored under src/core/vendor/) -- for models whose chat template is
 * too sophisticated for llama_chat_apply_template()'s plain built-in
 * matcher (llama.h documents that function as NOT using a real Jinja
 * parser, only a pre-defined list of known template formats). Gemma 4's
 * template is a real Jinja2 program (macros, loops, native tool/function
 * JSON-schema formatting) that isn't on that list -- this module is what
 * local_llm_client.c falls back to when the plain path fails.
 *
 * Implemented in jinja_chat_template.cpp (C++17, extern "C" linkage) --
 * this header is the only thing any C translation unit needs to see.
 */

#ifndef LEXIS_JINJA_CHAT_TEMPLATE_H
#define LEXIS_JINJA_CHAT_TEMPLATE_H

#include <stddef.h>

#include "local_llm_client.h" /* for LocalLlmTurn */

#ifdef __cplusplus
extern "C" {
#endif

/* Renders `turns[0..count)` through `jinja_template_src` (the model's own
 * raw Jinja chat template, as returned by llama_model_chat_template())
 * using minja. `bos_token`/`eos_token` come from the model's vocab (some
 * templates reference them directly, e.g. Gemma 4's `{{- bos_token -}}`).
 * `enable_thinking` is passed straight through as the template's own
 * `enable_thinking` context variable when `has_enable_thinking_override`
 * is non-zero -- most modern templates (Gemma 4, Qwen3-family) branch on
 * it directly, no prefill-string hack needed for this path. When
 * `has_enable_thinking_override` is 0, no `enable_thinking` value is
 * injected at all and the template's own default (if any) applies.
 *
 * Returns a newly malloc()'d string (caller must free()), or NULL on
 * failure (unparseable template, rendering error, or allocation
 * failure). */
char *jinja_render_chat_template(const char *jinja_template_src, const char *bos_token, const char *eos_token,
                                  const LocalLlmTurn *turns, size_t count, int add_generation_prompt,
                                  int enable_thinking, int has_enable_thinking_override);

#ifdef __cplusplus
}
#endif

#endif /* LEXIS_JINJA_CHAT_TEMPLATE_H */
