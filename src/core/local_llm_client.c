/*
 * Implementation of the llama.cpp-backed local model client.
 * See include/local_llm_client.h for the module's role.
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "local_llm_client.h"

#include "jinja_chat_template.h"
#include "string_builder.h"

#include <llama.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LOCAL_LLM_N_CTX is declared in local_llm_client.h, not here -- the
 * windowing helpers in query_formulation.c/generation.c need the real
 * ceiling to compute how much chat history budget they have, not a
 * number they'd have to keep in sync with this file by hand. */
#define LOCAL_LLM_N_BATCH LOCAL_LLM_N_CTX
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

/* Forward declaration -- defined later in this file, needed here so
 * local_llm_client_init() can warm its cache (see the call site below
 * for why). */
static char *apply_chat_template_multi(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                        int32_t *out_len);

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

    /* Prime the chat-template path now, during startup, rather than
     * paying for it on the user's first real chat message. Models whose
     * template needs the Jinja fallback (see apply_chat_template_multi()
     * below and jinja_chat_template.cpp's own comment) pay real parse
     * cost the first time it runs -- measured at ~11-12 seconds for
     * Gemma 4's real Jinja2 template -- which the fallback's own cache
     * then eliminates for every call after. Without this warm-up, that
     * one-time cost would land as a jarring delay on the very first
     * question a user asks instead of overlapping with the ~9-19s model
     * load this function already costs (same reasoning ModelLoader.h
     * documents for why the app loads the model proactively at startup
     * rather than deferring to first use). For models on the plain
     * built-in template path (Llama, Qwen), this warm-up is
     * essentially free. Result is discarded either way -- this call
     * exists purely for its cache side effect; if it fails for some
     * transient reason, the real call later just tries again on its own
     * merits. */
    LocalLlmTurn warmup_turn = {.role = "user", .content = "hi"};
    int32_t warmup_len = 0;
    char *warmup_result = apply_chat_template_multi(&warmup_turn, 1, NULL, &warmup_len);
    free(warmup_result);

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

/* Formats `turns[0..count)` with the model's own chat template into a
 * heap buffer (caller must free()), with `prefill` (if non-NULL) already
 * folded in -- one complete, ready-to-tokenize prompt either way.
 * Returns NULL on failure.
 *
 * Tries llama_chat_apply_template() first (handles the "buffer too
 * small" case it signals by reporting a formatted_len larger than the
 * buffer it was given -- grows once and re-applies, per the API's
 * documented contract). That function is NOT a real Jinja parser --
 * llama.h documents it as only supporting a pre-defined list of known
 * template formats -- so on a genuine failure (a model whose template is
 * too sophisticated for that list, e.g. Gemma 4's native-tool-calling-
 * capable template, confirmed directly by reading its raw Jinja source)
 * this falls back to real Jinja rendering via jinja_render_chat_template()
 * (minja, vendored under src/core/vendor/).
 *
 * The two paths handle `prefill` differently: the plain path appends it
 * as a literal string after the template's own assistant-turn opening
 * (see local_llm_chat_completion_multi()'s header doc comment on why
 * that skips a thinking model's reasoning pass). The Jinja path instead
 * passes `enable_thinking` as a real template context variable -- modern
 * templates like Gemma 4's branch on it directly, so the literal prefill
 * string isn't needed (and Gemma 4's template defaults enable_thinking
 * to false on its own, confirmed by reading its source -- so prefill ==
 * NULL going through this path still means "let the template's own
 * default apply", not "force thinking on"). */
static char *apply_chat_template_multi(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                        int32_t *out_len) {
    struct llama_chat_message *msgs = malloc(count * sizeof(struct llama_chat_message));
    if (msgs == NULL) {
        return NULL;
    }
    size_t content_len = 0;
    for (size_t i = 0; i < count; i++) {
        msgs[i].role = turns[i].role;
        msgs[i].content = turns[i].content;
        content_len += strlen(turns[i].content);
    }

    const char *tmpl = llama_model_chat_template(g_model, NULL);

    size_t buf_size = content_len * 2 + 256;
    char *formatted = malloc(buf_size);
    if (formatted == NULL) {
        free(msgs);
        return NULL;
    }

    int32_t formatted_len = llama_chat_apply_template(tmpl, msgs, count, true, formatted, (int32_t)buf_size);

    if (formatted_len >= 0 && (size_t)formatted_len > buf_size) {
        char *bigger = realloc(formatted, (size_t)formatted_len);
        if (bigger == NULL) {
            free(formatted);
            free(msgs);
            return NULL;
        }
        formatted = bigger;
        buf_size = (size_t)formatted_len;
        formatted_len = llama_chat_apply_template(tmpl, msgs, count, true, formatted, (int32_t)buf_size);
    }
    free(msgs);

    if (formatted_len < 0) {
        free(formatted);

        const char *bos = llama_vocab_get_text(g_vocab, llama_vocab_bos(g_vocab));
        const char *eos = llama_vocab_get_text(g_vocab, llama_vocab_eos(g_vocab));
        char *jinja_result = jinja_render_chat_template(tmpl, bos, eos, turns, count, /*add_generation_prompt=*/1,
                                                          /*enable_thinking=*/prefill != NULL ? 0 : 1,
                                                          /*has_enable_thinking_override=*/prefill != NULL ? 1 : 0);
        if (jinja_result == NULL) {
            fprintf(stderr,
                    "local_llm_chat_completion_multi: failed to apply chat template (plain built-in matcher and "
                    "Jinja fallback both failed)\n");
            return NULL;
        }
        *out_len = (int32_t)strlen(jinja_result);
        return jinja_result;
    }

    if (prefill == NULL) {
        *out_len = formatted_len;
        return formatted;
    }

    size_t prefill_len = strlen(prefill);
    char *with_prefill = malloc((size_t)formatted_len + prefill_len + 1);
    if (with_prefill == NULL) {
        free(formatted);
        return NULL;
    }
    memcpy(with_prefill, formatted, (size_t)formatted_len);
    memcpy(with_prefill + formatted_len, prefill, prefill_len + 1);
    free(formatted);

    *out_len = formatted_len + (int32_t)prefill_len;
    return with_prefill;
}

/* Shared greedy-decode loop, run against an already-tokenized prompt
 * (`tokens[0..n_tokens)`, already sized against LOCAL_LLM_N_CTX by the
 * caller). Returns the generated reply text (caller must free(), never
 * NULL on success -- an empty string is a valid reply, see below), or
 * NULL on failure. Does not free `tokens` -- the caller owns it. */
static char *run_decode_loop(llama_token *tokens, int32_t n_tokens) {
    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    struct llama_sampler *sampler = llama_sampler_chain_init(sparams);
    if (sampler == NULL) {
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

/* Strips a leading <think>...</think> block (plus any whitespace right
 * after it), in place. Defense-in-depth against a thinking model's
 * reasoning leaking into a caller's displayed/persisted output --
 * `prefill` (see above) is how a call *tries* to suppress the block in
 * the first place, but it isn't a hard guarantee the model won't still
 * open a fresh <think> tag on its own, so this runs regardless of
 * whether `prefill` was used. Only strips a block that starts at the
 * very beginning of `reply` and actually closes -- if there's no
 * <think>...</think> at all (the common case once every call site here
 * either prefills or the model just doesn't open one), or an opened
 * block never closes (e.g. generation hit LOCAL_LLM_MAX_NEW_TOKENS
 * mid-thought), `reply` is left untouched rather than guessed at. */
static void strip_leading_think_block(char *reply) {
    static const char *open_tag = "<think>";
    static const char *close_tag = "</think>";
    size_t open_len = strlen(open_tag);

    if (strncmp(reply, open_tag, open_len) != 0) {
        return;
    }
    char *close = strstr(reply + open_len, close_tag);
    if (close == NULL) {
        return;
    }

    char *after = close + strlen(close_tag);
    while (*after == '\n' || *after == '\r' || *after == ' ' || *after == '\t') {
        after++;
    }
    memmove(reply, after, strlen(after) + 1); /* +1 to carry the NUL terminator along */
}

char *local_llm_chat_completion_multi(const LocalLlmTurn *turns, size_t count, const char *prefill) {
    if (!g_initialized) {
        fprintf(stderr, "local_llm_chat_completion_multi: module not initialized\n");
        return NULL;
    }
    if (count == 0) {
        return NULL;
    }

    /* Each call is a fresh conversation, not a continuation of whatever
     * the previous call left in the KV cache -- the full turn history is
     * always passed in explicitly (see the windowing helpers in
     * query_formulation.c/generation.c), so token positions must start
     * at 0 here or the model would see an unrelated earlier prompt
     * layered underneath this one. */
    llama_memory_clear(llama_get_memory(g_ctx), true);

    int32_t formatted_len = 0;
    char *formatted = apply_chat_template_multi(turns, count, prefill, &formatted_len);
    if (formatted == NULL) {
        return NULL;
    }

    int32_t n_tokens_max = formatted_len + 16;
    llama_token *tokens = malloc((size_t)n_tokens_max * sizeof(llama_token));
    if (tokens == NULL) {
        free(formatted);
        return NULL;
    }

    int32_t n_tokens = llama_tokenize(g_vocab, formatted, formatted_len, tokens, n_tokens_max, true, true);
    free(formatted);
    if (n_tokens < 0) {
        fprintf(stderr, "local_llm_chat_completion_multi: tokenization failed\n");
        free(tokens);
        return NULL;
    }
    if (n_tokens >= LOCAL_LLM_N_CTX) {
        fprintf(stderr,
                "local_llm_chat_completion_multi: prompt (%d tokens) exceeds the %d-token context "
                "window\n",
                n_tokens, LOCAL_LLM_N_CTX);
        free(tokens);
        return NULL;
    }

    char *reply = run_decode_loop(tokens, n_tokens);
    free(tokens);
    if (reply != NULL) {
        strip_leading_think_block(reply);
    }
    return reply;
}

char *local_llm_chat_completion(const char *user_message) {
    LocalLlmTurn turn = {.role = "user", .content = user_message};
    return local_llm_chat_completion_multi(&turn, 1, NULL);
}

int local_llm_count_tokens(const char *text) {
    if (!g_initialized) {
        fprintf(stderr, "local_llm_count_tokens: module not initialized\n");
        return -1;
    }

    int32_t text_len = (int32_t)strlen(text);
    int32_t n_tokens_max = text_len + 16;
    llama_token *tokens = malloc((size_t)n_tokens_max * sizeof(llama_token));
    if (tokens == NULL) {
        return -1;
    }

    /* add_special = false -- this counts one turn's own content toward a
     * windowing budget, not a full templated prompt (which gets exactly
     * one BOS token regardless of how many turns it's made of, added by
     * apply_chat_template_multi()/llama_tokenize() above, not here). */
    int32_t n_tokens = llama_tokenize(g_vocab, text, text_len, tokens, n_tokens_max, false, true);
    free(tokens);
    if (n_tokens < 0) {
        fprintf(stderr, "local_llm_count_tokens: tokenization failed\n");
        return -1;
    }
    return n_tokens;
}
