/* Streaming identity check -- the F3 tripwire from the spec: the
 * streaming entry point must return bit-identical output to the plain
 * one, given the same prompt, or the eval reproducibility guarantee is
 * broken. Not part of `make check` (it loads a 5GB model); run by hand:
 *
 *   ./build/test_stream_identity
 *
 * Loads the model named in config/lexis.conf's model_path (falling back
 * to LEXIS_DEFAULT_MODEL_PATH), so pointing the config at another model
 * checks that one instead. Both calls run greedy with the same prompt;
 * the streaming one additionally feeds a callback that accumulates the
 * pieces, which lets this also assert that the concatenated stream
 * equals the returned answer exactly.
 *
 * Run twice: once with thinking off, once forced ON. The forced pass is
 * the one that exercises the streaming think gate -- suppressing the
 * reasoning block live while the returned answer is stripped by
 * strip_leading_think_block() -- including the whitespace run after the
 * close marker, which the gate has to skip across a piece boundary.
 */
#include "config.h"
#include "local_llm_client.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Accumulates streamed pieces into a malloc'd buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int calls;
} StreamAccumulator;

static void stream_sink(const char *piece, size_t piece_len, void *user_data) {
    StreamAccumulator *acc = user_data;
    if (acc->len + piece_len + 1 > acc->cap) {
        size_t new_cap = (acc->len + piece_len + 1) * 2;
        char *grown = realloc(acc->data, new_cap);
        if (grown == NULL) {
            return; /* OOM in a test harness: fail loudly below */
        }
        acc->data = grown;
        acc->cap = new_cap;
    }
    memcpy(acc->data + acc->len, piece, piece_len);
    acc->len += piece_len;
    acc->data[acc->len] = '\0';
    acc->calls++;
}

/* One pass: same prompt through both entry points, compared three ways.
 * Returns the number of failures. */
static int check_identity(int force_thinking) {
    const char *label = force_thinking ? "thinking ON" : "thinking off";
    LocalLlmTurn turn = {.role = "user", .content = "In one short sentence: what is BM25 search?"};

    /* Plain path. */
    char *plain = local_llm_chat_completion_multi_ex(&turn, 1, NULL, force_thinking);
    if (plain == NULL) {
        fprintf(stderr, "FAIL [%s]: plain completion failed\n", label);
        return 1;
    }

    /* Streaming path, same prompt and settings. */
    StreamAccumulator acc = {NULL, 0, 0, 0};
    char *streamed = local_llm_chat_completion_multi_ex_stream(&turn, 1, NULL, force_thinking, stream_sink, &acc);
    if (streamed == NULL) {
        fprintf(stderr, "FAIL [%s]: streaming completion failed\n", label);
        free(plain);
        return 1;
    }

    int failures = 0;
    if (strcmp(plain, streamed) != 0) {
        fprintf(stderr, "FAIL [%s]: returned answers differ\n  plain:    %s\n  streamed: %s\n", label, plain,
                streamed);
        failures++;
    } else {
        printf("[%s] returned answers identical (%zu bytes)\n", label, strlen(plain));
    }

    /* The pieces must rebuild the answer byte for byte -- no think-block
     * bytes leaking through, and no leading whitespace the returned
     * answer doesn't have (strip_leading_think_block() eats the run
     * after the close marker; the gate has to do the same even when that
     * run is split across pieces). */
    const char *accumulated = acc.data != NULL ? acc.data : "";
    if (strcmp(plain, accumulated) != 0) {
        fprintf(stderr, "FAIL [%s]: streamed pieces don't reconstruct the answer\n  returned: %s\n  streamed: %s\n",
                label, plain, accumulated);
        failures++;
    } else {
        printf("[%s] streamed pieces reconstruct the answer exactly (%d pieces)\n", label, acc.calls);
    }

    free(plain);
    free(streamed);
    free(acc.data);
    return failures;
}

int main(void) {
    /* The config's model, not a hardcoded one: the Makefile target says
     * so, and a config pointing elsewhere should check that model. */
    char *model_path = config_load_model_path(lexis_paths_config_file());
    if (model_path == NULL) {
        fprintf(stderr, "could not resolve a model path\n");
        return 1;
    }
    printf("model: %s\n", model_path);
    if (local_llm_client_init(model_path) != 0) {
        fprintf(stderr, "model init failed: %s\n", model_path);
        free(model_path);
        return 1;
    }
    free(model_path);

    int failures = check_identity(/*force_thinking=*/0);
    failures += check_identity(/*force_thinking=*/1);

    local_llm_client_cleanup();
    if (failures > 0) {
        return 1;
    }
    printf("stream identity: OK\n");
    return 0;
}