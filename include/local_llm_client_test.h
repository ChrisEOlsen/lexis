/* Test hooks for the streaming think gate. Test-only: production code drives the
 * gate internally and must not call these. THINK_GATE_FORMAT_COUNT must match
 * kThinkFormats in local_llm_client.c (enforced there by _Static_assert). */

#ifndef LEXIS_LOCAL_LLM_CLIENT_TEST_H
#define LEXIS_LOCAL_LLM_CLIENT_TEST_H

#include <stddef.h>

#include "local_llm_client.h"
#include "string_builder.h"

#define THINK_GATE_FORMAT_COUNT 2

typedef struct {
    int phase_deciding;
    int phase_inside;
    size_t format;
    size_t open_match[THINK_GATE_FORMAT_COUNT];
    size_t close_scanned;
    int skip_leading_ws;
    StringBuilder held;
} ThinkGate;

void think_gate_feed(ThinkGate *gate, const char *piece, LocalLlmStreamFn on_piece, void *user_data);
void think_gate_flush(ThinkGate *gate, LocalLlmStreamFn on_piece, void *user_data);
void strip_leading_think_block(char *reply);

#endif /* LEXIS_LOCAL_LLM_CLIENT_TEST_H */
