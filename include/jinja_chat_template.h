/* C bridge to a real Jinja2 chat-template renderer (minja); fallback when llama's matcher fails. */

#ifndef LEXIS_JINJA_CHAT_TEMPLATE_H
#define LEXIS_JINJA_CHAT_TEMPLATE_H

#include <stddef.h>

#include "local_llm_client.h" /* for LocalLlmTurn */

#ifdef __cplusplus
extern "C" {
#endif

/* Render turns through the model's raw Jinja template (bos/eos from vocab). Override 0 = template default.
 * Caller frees; NULL on template/render/alloc failure. */
char *jinja_render_chat_template(const char *jinja_template_src, const char *bos_token, const char *eos_token,
                                  const LocalLlmTurn *turns, size_t count, int add_generation_prompt,
                                  int enable_thinking, int has_enable_thinking_override);

#ifdef __cplusplus
}
#endif

#endif /* LEXIS_JINJA_CHAT_TEMPLATE_H */
