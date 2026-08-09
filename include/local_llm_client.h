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

#include <stddef.h>

/* The model's inference context window, in tokens -- large enough for
 * TOP_K=5 passages at ~200 tokens/chunk plus prompt scaffolding (well
 * under 2K tokens) AND several turns of windowed chat history (see
 * query_formulation_contextualize_question()/
 * generation_generate_answer_with_history()'s windowing helpers, which
 * need this real ceiling to compute their budgets against). This model's
 * GGUF metadata reports n_ctx_train = 131072, so 16384 is still a small,
 * deliberately conservative fraction of what it actually supports,
 * chosen against real KV-cache memory math (28 layers x 8 KV heads x
 * 128-dim x 2 bytes(F16) x 2(K+V) per token =~112KB/token, so 16384
 * tokens =~1.8GB of KV cache) fitting comfortably alongside the ~5.7GB
 * Metal working-set budget measured on the dev machine (see
 * LIMITATIONS.md). Declared here, not just in local_llm_client.c, so
 * every caller that needs to budget against it shares one number rather
 * than each hardcoding its own copy. */
#define LOCAL_LLM_N_CTX 16384

/* One turn of a conversation, in the model's own chat-template terms --
 * `role` is "user" or "assistant" (the two roles this project ever sends;
 * no "system" turn is used anywhere yet). Both fields are borrowed, never
 * freed by this module -- the caller owns their lifetime for the duration
 * of the local_llm_chat_completion_multi() call they're passed to. */
typedef struct {
    const char *role;
    const char *content;
} LocalLlmTurn;

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
 * generation fails. Equivalent to calling
 * local_llm_chat_completion_multi() with a single "user" turn. */
char *local_llm_chat_completion(const char *user_message);

/* Multi-turn counterpart: formats `turns[0..count)` (in order, oldest
 * first, alternating "user"/"assistant") using the model's own chat
 * template -- the real per-role template markup, not history flattened
 * into one string -- then greedily decodes a reply to the implied next
 * turn. `count` must be >= 1. Same failure contract as
 * local_llm_chat_completion(): NULL if uninitialized, the formatted
 * prompt doesn't fit in the context window, or generation fails.
 *
 * `prefill`, if non-NULL, is appended to the templated prompt (after the
 * chat template's own assistant-turn opening) before tokenizing --
 * verified as the way to skip a thinking model's reasoning pass for a
 * given call at zero extra latency and no template-engine work: passing
 * "<think>\n\n</think>\n\n" makes the model see an already-closed, empty
 * reasoning block as context it doesn't need to (re-)generate, so
 * continuation goes straight to the real answer. Pass NULL for normal
 * behavior (whatever the model's chat template does by default). */
char *local_llm_chat_completion_multi(const LocalLlmTurn *turns, size_t count, const char *prefill);

/* Same, with explicit control over the model's reasoning pass.
 *
 * `force_thinking` non-zero renders the chat template with
 * enable_thinking = true AND the override flag set. Both are needed: passing
 * prefill == NULL alone does NOT turn thinking on, because that path leaves
 * the override unset and Gemma 4's template defaults enable_thinking to
 * false on its own. Any reasoning block the model then emits is stripped
 * from the returned reply, so callers still get just the answer.
 *
 * Exists to make "does reasoning improve grounded answers?" a measurable
 * question rather than an assumption -- every generation call in this project
 * currently disables it for latency. */
char *local_llm_chat_completion_multi_ex(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                          int force_thinking);

/* Tokenizes `text` (module's own vocabulary, no BOS/special tokens added
 * -- this counts one turn's own content toward a windowing budget, not a
 * full templated prompt) and returns the token count, or -1 if the
 * module hasn't been initialized or tokenization fails. Used by
 * query_formulation_contextualize_question()/
 * generation_generate_answer_with_history()'s windowing helpers to decide
 * how much conversation history fits under LOCAL_LLM_N_CTX. */
int local_llm_count_tokens(const char *text);

#endif /* LEXIS_LOCAL_LLM_CLIENT_H */
