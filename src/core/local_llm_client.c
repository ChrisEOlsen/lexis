/* llama.cpp-backed local model client (see local_llm_client.h). */

/* Must precede #includes: strdup is POSIX, hidden under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "local_llm_client.h"

#include "jinja_chat_template.h"
#include "local_llm_client_test.h"
#include "string_builder.h"

#include <llama.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* N_CTX stays in the header: windowing helpers in query_formulation.c/generation.c
 * need the real ceiling without hand-syncing. */
#define LOCAL_LLM_N_BATCH LOCAL_LLM_N_CTX
/* MAX_NEW_TOKENS also lives in the header: callers size reservations against it. */

static struct llama_model *g_model = NULL;
static struct llama_context *g_ctx = NULL;
static const struct llama_vocab *g_vocab = NULL;
static int g_initialized = 0;

/* Suppresses llama.cpp/ggml INFO/DEBUG noise (Metal/backend chatter); WARN/ERROR
 * still reach stderr so real problems aren't hidden. */
static void local_llm_log_callback(enum ggml_log_level level, const char *text, void *user_data) {
    (void)user_data;
    if (level >= GGML_LOG_LEVEL_WARN) {
        fprintf(stderr, "%s", text);
    }
}

/* Defined below; declared here so init() can warm its cache. */
static char *apply_chat_template_multi(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                        int force_thinking, int32_t *out_len);

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

    /* Warm the chat-template path now: first Jinja-fallback render costs ~11-12s (Gemma 4)
     * but caches; without this it would land on the user's first question. Discarded. */
    LocalLlmTurn warmup_turn = {.role = "user", .content = "hi"};
    int32_t warmup_len = 0;
    char *warmup_result = apply_chat_template_multi(&warmup_turn, 1, NULL, /*force_thinking=*/0, &warmup_len);
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

/* Formats turns (+prefill) via the model's chat template (caller frees, NULL on fail).
 * Built-in apply first (grows once if short); minja fallback for real-Jinja templates. */
static char *apply_chat_template_multi(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                        int force_thinking, int32_t *out_len) {
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
                                                          /*enable_thinking=*/force_thinking ? 1 : (prefill != NULL ? 0 : 1),
                                                          /*has_enable_thinking_override=*/
                                                          (force_thinking || prefill != NULL) ? 1 : 0);
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

/* Strips one leading reasoning block in place (<think> or Gemma 4's <|channel>thought,
 * whose asymmetric closing <channel|> is NOT a typo). Unterminated blocks stay visible. */
static const struct {
    const char *open;
    const char *close;
} kThinkFormats[] = {
    {"<think>", "</think>"},
    {"<|channel>thought", "<channel|>"},
};

/* Non-static: exposed to tests via local_llm_client_test.h. */
void strip_leading_think_block(char *reply) {
    for (size_t i = 0; i < sizeof(kThinkFormats) / sizeof(kThinkFormats[0]); i++) {
        size_t open_len = strlen(kThinkFormats[i].open);
        if (strncmp(reply, kThinkFormats[i].open, open_len) != 0) {
            continue;
        }
        char *close = strstr(reply + open_len, kThinkFormats[i].close);
        if (close == NULL) {
            return; /* opened but never closed -- leave it visible */
        }
        char *after = close + strlen(kThinkFormats[i].close);
        while (*after == '\n' || *after == '\r' || *after == ' ' || *after == '\t') {
            after++;
        }
        memmove(reply, after, strlen(after) + 1); /* +1 carries the NUL along */
        return;
    }
}

/* ThinkGate lives in local_llm_client_test.h (test hooks); the gate holds bytes until
 * the opening resolves (DECIDING -> INSIDE -> release), flushes truncated blocks
 * visible, and the returned reply is still stripped. */
_Static_assert(THINK_GATE_FORMAT_COUNT == sizeof(kThinkFormats) / sizeof(kThinkFormats[0]),
               "test header out of sync with kThinkFormats");

/* The whitespace strip_leading_think_block() skips after a close marker. */
static int think_gap_ws(char c) {
    return c == '\n' || c == '\r' || c == ' ' || c == '\t';
}

#define THINK_GATE_RULED_OUT ((size_t)-1)

/* Feeds one decoded piece through the gate, invoking `on_piece` with
 * whatever the gate decides is now displayable answer text. */
/* Non-static: exposed to tests via local_llm_client_test.h. */
void think_gate_feed(ThinkGate *gate, const char *piece, LocalLlmStreamFn on_piece, void *user_data) {
    /* Released state: pure pass-through WITHOUT touching held (appending would make
     * the flush re-emit the whole answer as a duplicate final piece). */
    if (!gate->phase_deciding && !gate->phase_inside) {
        size_t len = strlen(piece);
        size_t start = 0;
        if (gate->skip_leading_ws) {
            while (start < len && think_gap_ws(piece[start])) {
                start++;
            }
            if (start == len) {
                return; /* still inside the run: nothing displayable yet */
            }
            gate->skip_leading_ws = 0;
        }
        if (on_piece != NULL) {
            on_piece(piece + start, len - start, user_data);
        }
        return;
    }

    if (string_builder_append(&gate->held, piece) != 0) {
        return; /* allocation failure: the loop's own append will fail and abort the call */
    }

    if (gate->phase_deciding) {
        size_t held_len = gate->held.length;
        int any_alive = 0;
        int matched = -1;
        for (size_t f = 0; f < THINK_GATE_FORMAT_COUNT; f++) {
            if (gate->open_match[f] == THINK_GATE_RULED_OUT) {
                continue;
            }
            size_t open_len = strlen(kThinkFormats[f].open);
            /* Extend this format's matched prefix over the new bytes. */
            while (gate->open_match[f] < open_len && gate->open_match[f] < held_len &&
                   gate->held.data[gate->open_match[f]] == kThinkFormats[f].open[gate->open_match[f]]) {
                gate->open_match[f]++;
            }
            if (gate->open_match[f] >= open_len) {
                matched = (int)f; /* the full open marker has been consumed */
                break;
            }
            if (gate->open_match[f] == held_len) {
                /* Held text is still a strict prefix of this open
                 * marker -- can't rule it out yet. */
                any_alive = 1;
            } else {
                /* Held text diverged from this marker inside its
                 * length: ruled out permanently. */
                gate->open_match[f] = THINK_GATE_RULED_OUT;
            }
        }

        if (matched >= 0) {
            gate->phase_deciding = 0;
            gate->phase_inside = 1;
            gate->format = (size_t)matched;
            gate->close_scanned = 0;
            /* Open marker stays in held (dropped with the block). Fall through to the
             * INSIDE scan: this piece may already carry the close marker too. */
        } else {
            if (!any_alive) {
                /* An ordinary answer: release everything held and stream
                 * every later piece straight through. */
                gate->phase_deciding = 0;
                if (on_piece != NULL && gate->held.length > 0) {
                    on_piece(gate->held.data, gate->held.length, user_data);
                }
                free(gate->held.data);
                gate->held.data = NULL;
                gate->held.length = 0;
                gate->held.capacity = 0;
            }
            return;
        }
    }

    if (gate->phase_inside) {
        const char *close = kThinkFormats[gate->format].close;
        size_t close_len = strlen(close);
        size_t held_len = gate->held.length;
        size_t from = gate->close_scanned >= close_len - 1 ? gate->close_scanned - (close_len - 1) : 0;
        for (size_t i = from; i + close_len <= held_len; i++) {
            if (memcmp(gate->held.data + i, close, close_len) == 0) {
                size_t after = i + close_len;
                while (after < held_len && think_gap_ws(gate->held.data[after])) {
                    after++;
                }
                gate->phase_inside = 0;
                if (after < held_len) {
                    if (on_piece != NULL) {
                        on_piece(gate->held.data + after, held_len - after, user_data);
                    }
                } else {
                    /* Held text ends where the skip does; the run may continue next piece,
                     * which the non-streaming strip would also eat -- keep skipping. */
                    gate->skip_leading_ws = 1;
                }
                free(gate->held.data);
                gate->held.data = NULL;
                gate->held.length = 0;
                gate->held.capacity = 0;
                return;
            }
        }
        gate->close_scanned = held_len;
    }
}

/* Releases whatever is still held; called once at generation end. Opened-but-never-
 * closed stays visible (same rule as strip_leading_think_block()). */
/* Non-static: exposed to tests via local_llm_client_test.h. */
void think_gate_flush(ThinkGate *gate, LocalLlmStreamFn on_piece, void *user_data) {
    if (gate->held.data != NULL && gate->held.length > 0 && on_piece != NULL) {
        on_piece(gate->held.data, gate->held.length, user_data);
    }
    free(gate->held.data);
    gate->held.data = NULL;
    gate->held.length = 0;
    gate->held.capacity = 0;
}

/* Greedy-decode loop over a tokenized, ctx-sized prompt (tokens stay caller-owned).
 * Returns malloc'd reply ("" is valid, NULL is failure); on_piece gets live pieces. */
static char *run_decode_loop(llama_token *tokens, int32_t n_tokens, LocalLlmStreamFn on_piece, void *user_data) {
    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    struct llama_sampler *sampler = llama_sampler_chain_init(sparams);
    if (sampler == NULL) {
        return NULL;
    }
    /* Greedy (always the highest-probability token) -- deterministic and
     * cheap, matching the "fast and grounded" goal over creative variety. */
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    ThinkGate gate;
    gate.phase_deciding = on_piece != NULL ? 1 : 0;
    gate.phase_inside = 0;
    gate.format = 0;
    for (size_t f = 0; f < THINK_GATE_FORMAT_COUNT; f++) {
        gate.open_match[f] = 0;
    }
    gate.close_scanned = 0;
    gate.skip_leading_ws = 0;
    gate.held.data = NULL;
    gate.held.length = 0;
    gate.held.capacity = 0;

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
            if (on_piece != NULL) {
                think_gate_feed(&gate, piece, on_piece, user_data);
            }
        }

        n_ctx_used++;
        if (n_ctx_used >= LOCAL_LLM_N_CTX) {
            /* Out of context mid-generation: stop cleanly rather than overflowing KV cache. */
            break;
        }

        batch = llama_batch_get_one(&new_token, 1);
    }

    llama_sampler_free(sampler);

    if (on_piece != NULL) {
        think_gate_flush(&gate, on_piece, user_data);
    }

    if (!ok) {
        free(reply.data);
        return NULL;
    }

    if (reply.data == NULL) {
        /* First token was already EOG: valid empty reply, not failure (non-NULL = success). */
        return strdup("");
    }

    return reply.data;
}

/* One loop behind plain + streaming entry points (shared loop => provably identical
 * outputs; see local_llm_client.h). */
static char *chat_completion_multi_ex_common(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                             int force_thinking, LocalLlmStreamFn on_piece, void *user_data) {
    if (!g_initialized) {
        fprintf(stderr, "local_llm_chat_completion_multi: module not initialized\n");
        return NULL;
    }
    if (count == 0) {
        return NULL;
    }

    /* Fresh conversation per call, not a KV-cache continuation: full history is always
     * passed explicitly, so positions restart at 0. */
    llama_memory_clear(llama_get_memory(g_ctx), true);

    int32_t formatted_len = 0;
    char *formatted = apply_chat_template_multi(turns, count, prefill, force_thinking, &formatted_len);
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

    char *reply = run_decode_loop(tokens, n_tokens, on_piece, user_data);
    free(tokens);
    if (reply != NULL) {
        strip_leading_think_block(reply);
    }
    return reply;
}

char *local_llm_chat_completion_multi_ex(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                          int force_thinking) {
    return chat_completion_multi_ex_common(turns, count, prefill, force_thinking, NULL, NULL);
}

char *local_llm_chat_completion_multi_ex_stream(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                                int force_thinking, LocalLlmStreamFn on_piece, void *user_data) {
    return chat_completion_multi_ex_common(turns, count, prefill, force_thinking, on_piece, user_data);
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

    /* add_special=false: counts one turn's content toward a budget, not a full prompt
     * (the single BOS is added by the template/tokenize path, not here). */
    int32_t n_tokens = llama_tokenize(g_vocab, text, text_len, tokens, n_tokens_max, false, true);
    free(tokens);
    if (n_tokens < 0) {
        fprintf(stderr, "local_llm_count_tokens: tokenization failed\n");
        return -1;
    }
    return n_tokens;
}

char *local_llm_chat_completion_multi(const LocalLlmTurn *turns, size_t count, const char *prefill) {
    return local_llm_chat_completion_multi_ex(turns, count, prefill, /*force_thinking=*/0);
}
