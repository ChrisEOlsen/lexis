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
/* LOCAL_LLM_MAX_NEW_TOKENS now lives in local_llm_client.h -- callers have
 * to size their own context reservations against it. */

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

/* Strips a leading reasoning block (plus any whitespace right after it),
 * in place, so a thinking model's internal monologue never reaches a
 * caller's displayed or persisted output.
 *
 * Two formats are recognised, because the delimiters are per-model and
 * getting them wrong is silent -- the reasoning simply arrives as the
 * answer:
 *
 *   <think> ... </think>                    the widespread convention
 *   <|channel>thought ... <channel|>     Gemma 4's, taken from its own
 *                                        chat template (note the pipe
 *                                        moves to the other side on the
 *                                        closing tag -- it is NOT a typo,
 *                                        and the template's own
 *                                        strip_thinking macro splits on
 *                                        exactly that string)
 *
 * Only strips a block that starts at the very beginning of `reply` and
 * actually closes. An unterminated block -- generation having hit
 * LOCAL_LLM_MAX_NEW_TOKENS mid-thought -- is left untouched rather than
 * guessed at, so the truncation is visible instead of being silently
 * turned into a plausible-looking answer. */
static const struct {
    const char *open;
    const char *close;
} kThinkFormats[] = {
    {"<think>", "</think>"},
    {"<|channel>thought", "<channel|>"},
};

static void strip_leading_think_block(char *reply) {
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

/* -- Streaming think suppression ---------------------------------------
 *
 * The non-streaming path strips a leading reasoning block from the
 * completed reply above. A streaming caller must not receive those
 * bytes live -- the model's internal monologue would flash past the
 * user before the answer replaced it -- but "does this reply open with
 * a think block?" cannot be answered until either the block closes or
 * the reply provably does not start with any open marker. So the
 * decode loop holds pieces in a gate until that question resolves:
 *
 *   - DECIDING: bytes are compared against both open markers. Once
 *     they diverge from both (an ordinary answer -- the overwhelmingly
 *     common case, and always the case on the prefill path, where the
 *     template has already closed the think block in the prompt),
 *     everything held is released and every later piece flows through
 *     unchanged. Once one open marker fully matches, the gate goes
 *     INSIDE with that format.
 *   - INSIDE: pieces are held and scanned for the format's close
 *     marker. When it is found, everything up to and including it plus
 *     the whitespace run after it (the same run
 *     strip_leading_think_block() skips) is dropped, and the tail is
 *     released; the reply streams as plain answer text from there on.
 *   - Generation ends while still DECIDING or INSIDE -- a think block
 *     that opened but never closed, the truncated-mid-thought case --
 *     everything held is released as-is: the same "leave the truncation
 *     visible" rule the non-streaming strip applies, so a stream that
 *     dies mid-thought never looks like a clean empty answer.
 *
 * The final reply the loop returns is still stripped by
 * strip_leading_think_block(), so the returned string (what callers
 * persist and display) can never diverge from the non-streaming path;
 * the gate only governs which bytes reach the callback along the way. */
typedef struct {
    int phase_deciding; /* 1 until the reply's opening is understood */
    int phase_inside;   /* 1 once an open marker matched */
    size_t format;      /* index into kThinkFormats once known */
    /* DECIDING only: matched-prefix length against each open marker,
     * or SIZE_MAX for a format the held text has already diverged from. */
    size_t open_match[2];
    /* INSIDE only: bytes of the held text already scanned for the close
     * marker -- the next scan starts close_len-1 bytes earlier so a
     * marker split across a piece boundary is still found. */
    size_t close_scanned;
    /* Set when the gate leaves INSIDE with the whitespace run after the
     * close marker still unfinished -- the run continues into the next
     * piece, and strip_leading_think_block() skips it there too. Stays
     * set through pass-through pieces until real answer text arrives. */
    int skip_leading_ws;
    /* The held buffer: reply bytes whose disposition is not yet
     * decided. In DECIDING this is everything; in INSIDE it is the
     * think block body; once released, it is empty. */
    StringBuilder held;
} ThinkGate;

/* The whitespace strip_leading_think_block() skips after a close marker. */
static int think_gap_ws(char c) {
    return c == '\n' || c == '\r' || c == ' ' || c == '\t';
}

#define THINK_GATE_FORMAT_COUNT (sizeof(kThinkFormats) / sizeof(kThinkFormats[0]))
#define THINK_GATE_RULED_OUT ((size_t)-1)

/* Feeds one decoded piece through the gate, invoking `on_piece` with
 * whatever the gate decides is now displayable answer text. */
static void think_gate_feed(ThinkGate *gate, const char *piece, LocalLlmStreamFn on_piece, void *user_data) {
    /* Released state first, and WITHOUT touching `held`: once the gate
     * has released, holding is over, and appending pieces to the buffer
     * anyway would make think_gate_flush() re-emit the whole answer as
     * one final duplicate piece (found by the stream-identity check).
     * Pass-through is pure: the piece goes to the callback and nothing
     * else. */
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
            /* The open marker stays in `held` and is dropped along with
             * the whole think block when the close marker is found.
             * Deliberately NOT returning: this same piece may already
             * carry the close marker (a short block whose open and close
             * land in one piece), and the INSIDE scan below is what
             * finds it. Returning here would leave that block held until
             * some later piece arrived -- and if none did, the flush
             * would emit the entire reasoning block. */
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
                    /* The held text ends exactly where the skip does --
                     * either the close marker landed on a piece boundary
                     * or the run consumed the rest. The run may continue
                     * in the next piece; the non-streaming strip would
                     * eat that too, so keep skipping rather than passing
                     * whitespace the returned answer does not have. */
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

/* Releases whatever the gate is still holding. Called exactly once when
 * generation ends, before the reply is returned -- the
 * opened-but-never-closed case must stay visible, matching
 * strip_leading_think_block()'s rule. */
static void think_gate_flush(ThinkGate *gate, LocalLlmStreamFn on_piece, void *user_data) {
    if (gate->held.data != NULL && gate->held.length > 0 && on_piece != NULL) {
        on_piece(gate->held.data, gate->held.length, user_data);
    }
    free(gate->held.data);
    gate->held.data = NULL;
    gate->held.length = 0;
    gate->held.capacity = 0;
}

/* Shared greedy-decode loop, run against an already-tokenized prompt
 * (`tokens[0..n_tokens)`, already sized against LOCAL_LLM_N_CTX by the
 * caller). Returns the generated reply text (caller must free(), never
 * NULL on success -- an empty string is a valid reply, see below), or
 * NULL on failure. Does not free `tokens` -- the caller owns it.
 *
 * `on_piece` (when non-NULL) receives the reply's displayable answer
 * text live, piece by piece, post think-suppression (see ThinkGate);
 * the loop's returned buffer still carries the unstripped reply, which
 * the public entry points strip exactly as the non-streaming path
 * does. The two paths share this one loop -- the same sampler, the
 * same decode calls, the same caps -- so their outputs are bit-identical
 * by construction. */
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
            /* Ran out of context window mid-generation -- stop cleanly
             * with whatever's been produced so far rather than
             * overflowing the KV cache. */
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
        /* The first sampled token was already EOG -- a valid empty reply,
         * not a failure. Match the "non-NULL means success" contract with
         * an empty string rather than NULL. */
        return strdup("");
    }

    return reply.data;
}

/* One implementation behind both the plain and the streaming entry
 * points (see local_llm_client.h's streaming doc comment for why
 * sharing it is what makes their outputs provably identical). */
static char *chat_completion_multi_ex_common(const LocalLlmTurn *turns, size_t count, const char *prefill,
                                             int force_thinking, LocalLlmStreamFn on_piece, void *user_data) {
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

char *local_llm_chat_completion_multi(const LocalLlmTurn *turns, size_t count, const char *prefill) {
    return local_llm_chat_completion_multi_ex(turns, count, prefill, /*force_thinking=*/0);
}
