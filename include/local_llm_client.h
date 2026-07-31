/*
 * llama.cpp-backed local model client (spec 5.2.4, 5.2.7 -- replaces
 * openrouter_client.h as the LLM call site for both query formulation and
 * generation, per the project's move away from a paid external API). One
 * GGUF model is loaded once at startup and reused for every call, unlike
 * OpenRouter where each call names a model string -- there's only one
 * model loaded here, so callers don't pick one per request.
 */

#ifndef LEXIS_LOCAL_LLM_CLIENT_H
#define LEXIS_LOCAL_LLM_CLIENT_H

/* Loads the GGUF model at `model_path` into memory (offloaded to GPU via
 * Metal where available) and initializes the llama.cpp backend. Must be
 * called exactly once, before any other function in this module. Returns
 * 0 on success, -1 if the backend or model fails to load. */
int local_llm_client_init(const char *model_path);

/* Frees the loaded model and context and shuts down the llama.cpp
 * backend. Must be called exactly once, after every other call into this
 * module has finished. Safe to call even if init failed or was never
 * called. */
void local_llm_client_cleanup(void);

/* Sends a single-turn chat completion: formats `user_message` using the
 * model's own embedded chat template, then greedily decodes a reply
 * (deterministic -- no sampling randomness, matching the "not super
 * smart, just fast and grounded" goal over creative variety). Returns the
 * reply text (caller must free()), or NULL if the module hasn't been
 * initialized, the prompt doesn't fit in the context window, or
 * generation fails. */
char *local_llm_chat_completion(const char *user_message);

#endif /* LEXIS_LOCAL_LLM_CLIENT_H */
