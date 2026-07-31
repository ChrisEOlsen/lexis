/*
 * Implementation of the llama.cpp-backed local model client.
 * See include/local_llm_client.h for the module's role.
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "local_llm_client.h"

#include "string_builder.h"

#include <llama.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Large enough for LEXIS's actual prompts (TOP_K=5 passages at ~200
 * tokens/chunk plus prompt scaffolding, well under 2K tokens) with
 * comfortable headroom, without paying for a context bigger than this
 * pipeline will ever fill. n_batch matches n_ctx so any prompt that fits
 * in the context also fits in a single prefill decode() call -- no
 * chunked-prefill logic needed at this prompt size. See LIMITATIONS.md if
 * that assumption ever needs revisiting (e.g. a much larger TOP_K). */
#define LOCAL_LLM_N_CTX 4096
#define LOCAL_LLM_N_BATCH 4096
#define LOCAL_LLM_MAX_NEW_TOKENS 512

static struct llama_model *g_model = NULL;
static struct llama_context *g_ctx = NULL;
static const struct llama_vocab *g_vocab = NULL;
static int g_initialized = 0;

/* llama.cpp/ggml log INFO/DEBUG-level Metal shader compilation and
 * backend detection noise (hundreds of lines) on every single model
 * load by default. Real problems (WARN/ERROR) still reach stderr;
 * everything else is suppressed so a query's actual output isn't buried
 * under it. */
static void local_llm_log_callback(enum ggml_log_level level, const char *text, void *user_data) {
    (void)user_data;
    if (level >= GGML_LOG_LEVEL_WARN) {
        fprintf(stderr, "%s", text);
    }
}

int local_llm_client_init(const char *model_path) {
    llama_log_set(local_llm_log_callback, NULL);
    llama_backend_init();

    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 999; /* offload everything to GPU (Metal on Apple Silicon) */
    model_params.progress_callback = NULL; /* disable the default dot-per-percent load progress printer */

    g_model = llama_model_load_from_file(model_path, model_params);
    if (g_model == NULL) {
        fprintf(stderr, "local_llm_client_init: failed to load model from %s\n", model_path);
        llama_backend_free();
        return -1;
    }
    g_vocab = llama_model_get_vocab(g_model);

    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = LOCAL_LLM_N_CTX;
    ctx_params.n_batch = LOCAL_LLM_N_BATCH;

    g_ctx = llama_init_from_model(g_model, ctx_params);
    if (g_ctx == NULL) {
        fprintf(stderr, "local_llm_client_init: failed to create inference context\n");
        llama_model_free(g_model);
        g_model = NULL;
        llama_backend_free();
        return -1;
    }

    g_initialized = 1;
    return 0;
}

void local_llm_client_cleanup(void) {
    if (g_ctx != NULL) {
        llama_free(g_ctx);
        g_ctx = NULL;
    }
    if (g_model != NULL) {
        llama_model_free(g_model);
        g_model = NULL;
    }
    if (g_initialized) {
        llama_backend_free();
        g_initialized = 0;
    }
}

/* Formats `user_message` with the model's own chat template into a
 * heap buffer (caller must free()). Returns NULL on failure. Handles the
 * "buffer too small" case llama_chat_apply_template signals by reporting
 * a formatted_len larger than the buffer it was given -- grows once and
 * re-applies, per the API's documented contract. */
static char *apply_chat_template(const char *user_message, int32_t *out_len) {
    struct llama_chat_message msg = {.role = "user", .content = user_message};
    const char *tmpl = llama_model_chat_template(g_model, NULL);

    size_t buf_size = strlen(user_message) * 2 + 256;
    char *formatted = malloc(buf_size);
    if (formatted == NULL) {
        return NULL;
    }

    int32_t formatted_len =
        llama_chat_apply_template(tmpl, &msg, 1, true, formatted, (int32_t)buf_size);
    if (formatted_len < 0) {
        fprintf(stderr, "local_llm_chat_completion: failed to apply chat template\n");
        free(formatted);
        return NULL;
    }

    if ((size_t)formatted_len > buf_size) {
        char *bigger = realloc(formatted, (size_t)formatted_len);
        if (bigger == NULL) {
            free(formatted);
            return NULL;
        }
        formatted = bigger;
        buf_size = (size_t)formatted_len;
        formatted_len =
            llama_chat_apply_template(tmpl, &msg, 1, true, formatted, (int32_t)buf_size);
        if (formatted_len < 0) {
            free(formatted);
            return NULL;
        }
    }

    *out_len = formatted_len;
    return formatted;
}

char *local_llm_chat_completion(const char *user_message) {
    if (!g_initialized) {
        fprintf(stderr, "local_llm_chat_completion: module not initialized\n");
        return NULL;
    }

    /* Each call is a fresh single turn (matching openrouter_chat_completion's
     * "one prompt in, one reply out" contract) -- clear whatever KV cache
     * state a previous call left behind so token positions start at 0 and
     * the model doesn't see an unrelated earlier prompt as prior
     * conversation. */
    llama_memory_clear(llama_get_memory(g_ctx), true);

    int32_t formatted_len = 0;
    char *formatted = apply_chat_template(user_message, &formatted_len);
    if (formatted == NULL) {
        return NULL;
    }

    int32_t n_tokens_max = formatted_len + 16;
    llama_token *tokens = malloc((size_t)n_tokens_max * sizeof(llama_token));
    if (tokens == NULL) {
        free(formatted);
        return NULL;
    }

    int32_t n_tokens =
        llama_tokenize(g_vocab, formatted, formatted_len, tokens, n_tokens_max, true, true);
    free(formatted);
    if (n_tokens < 0) {
        fprintf(stderr, "local_llm_chat_completion: tokenization failed\n");
        free(tokens);
        return NULL;
    }
    if (n_tokens >= LOCAL_LLM_N_CTX) {
        fprintf(stderr,
                "local_llm_chat_completion: prompt (%d tokens) exceeds the %d-token context "
                "window\n",
                n_tokens, LOCAL_LLM_N_CTX);
        free(tokens);
        return NULL;
    }

    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    struct llama_sampler *sampler = llama_sampler_chain_init(sparams);
    if (sampler == NULL) {
        free(tokens);
        return NULL;
    }
    /* Greedy (always the highest-probability token) -- deterministic and
     * cheap, matching the "fast and grounded" goal over creative variety. */
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    StringBuilder reply = {NULL, 0, 0};
    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    int32_t n_ctx_used = n_tokens;
    int ok = 1;

    for (int i = 0; i < LOCAL_LLM_MAX_NEW_TOKENS; i++) {
        if (llama_decode(g_ctx, batch) != 0) {
            fprintf(stderr, "local_llm_chat_completion: decode failed\n");
            ok = 0;
            break;
        }

        llama_token new_token = llama_sampler_sample(sampler, g_ctx, -1);
        if (llama_vocab_is_eog(g_vocab, new_token)) {
            break;
        }

        char piece[256];
        int32_t piece_len = llama_token_to_piece(g_vocab, new_token, piece, sizeof(piece), 0, true);
        if (piece_len > 0) {
            piece[piece_len] = '\0';
            if (string_builder_append(&reply, piece) != 0) {
                ok = 0;
                break;
            }
        }

        n_ctx_used++;
        if (n_ctx_used >= LOCAL_LLM_N_CTX) {
            /* Ran out of context window mid-generation -- stop cleanly
             * with whatever's been produced so far rather than
             * overflowing the KV cache. */
            break;
        }

        batch = llama_batch_get_one(&new_token, 1);
    }

    llama_sampler_free(sampler);
    free(tokens);

    if (!ok) {
        free(reply.data);
        return NULL;
    }

    if (reply.data == NULL) {
        /* The first sampled token was already EOG -- a valid empty reply,
         * not a failure. Match the "non-NULL means success" contract with
         * an empty string rather than NULL. */
        return strdup("");
    }

    return reply.data;
}
