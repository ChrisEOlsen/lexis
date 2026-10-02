/* llama.cpp-backed local model client: one GGUF loaded at startup, reused for every call. */

#ifndef LEXIS_LOCAL_LLM_CLIENT_H
#define LEXIS_LOCAL_LLM_CLIENT_H

#include <stddef.h>

/* Inference context window in tokens. Shared here so every caller budgets against one number. */
#define LOCAL_LLM_N_CTX 16384

/* Hard cap on generated tokens per call. Callers windowing history must reserve room for it. */
#define LOCAL_LLM_MAX_NEW_TOKENS 2048

/* One conversation turn; role is "user" or "assistant". Both fields borrowed for the call. */
typedef struct {
    const char *role;
    const char *content;
} LocalLlmTurn;

/* Load the GGUF model; call exactly once first. 0 on success, -1 on load failure. */
int local_llm_client_init(const char *model_path);

/* Free the model/context; call exactly once last. Safe even if init failed or never ran. */
void local_llm_client_cleanup(void);

/* Single-turn greedy (deterministic) completion. Caller frees; NULL if uninit, too long, or failed. */
char *local_llm_chat_completion(const char *user_message);

/* Multi-turn counterpart; count >= 1. Same failure contract as the single-turn call.
 * Non-NULL prefill is appended after the assistant-turn opening (e.g. LEXIS_PREFILL_NO_THINK). */
char *local_llm_chat_completion_multi(const LocalLlmTurn *turns, size_t count, const char *prefill);

/* Same, with explicit reasoning-pass control. force_thinking renders enable_thinking = true.
 * Any emitted reasoning block is stripped from the reply. */
char *local_llm_chat_completion_multi_ex(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                          int force_thinking);

/* Streaming twin: same reply, plus live answer-text pieces via on_piece. Returned string is authoritative.
 * piece is NOT NUL-terminated and valid only during the callback; user_data passed through. */
typedef void (*LocalLlmStreamFn)(const char *piece, size_t piece_len, void *user_data);

char *local_llm_chat_completion_multi_ex_stream(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                                int force_thinking, LocalLlmStreamFn on_piece, void *user_data);

/* Token count of text (no BOS/special tokens) for windowing budgets. -1 if uninit or failed. */
int local_llm_count_tokens(const char *text);

#endif /* LEXIS_LOCAL_LLM_CLIENT_H */
