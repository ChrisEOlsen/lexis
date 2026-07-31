/*
 * Implementation of answer generation.
 * See include/generation.h for the module's role (spec 5.2.7).
 */

#include "generation.h"

#include "local_llm_client.h"
#include "string_builder.h"

#include <stdio.h>
#include <stdlib.h>

char *generation_build_prompt(const char *query_text, PgStore *store,
                               const BM25ResultSet *results) {
    if (results->count == 0) {
        return NULL;
    }

    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder,
            "You are answering a question using only the provided context. If the "
            "context doesn't contain enough information to answer, say so rather "
            "than guessing.\n\nContext:\n\n") != 0) {
        goto fail;
    }

    /* A passage_id failing to load is skipped, not fatal -- track how
     * many actually made it in, since a prompt with zero real context
     * (every passage failed to load) isn't a grounded answer at all. */
    size_t passages_included = 0;
    for (size_t i = 0; i < results->count; i++) {
        PgStorePassage *passage = pg_store_get_passage(store, results->items[i].passage_id);
        if (passage == NULL) {
            continue;
        }

        char chunk_label[32];
        snprintf(chunk_label, sizeof(chunk_label), "%d", passage->chunk_id);

        if (string_builder_append(&builder, "[Source: ") != 0 ||
            string_builder_append(&builder, passage->document_name) != 0 ||
            string_builder_append(&builder, ", chunk ") != 0 ||
            string_builder_append(&builder, chunk_label) != 0 ||
            string_builder_append(&builder, "]\n") != 0 ||
            string_builder_append(&builder, passage->text) != 0 ||
            string_builder_append(&builder, "\n\n") != 0) {
            pg_store_passage_free(passage);
            goto fail;
        }

        pg_store_passage_free(passage);
        passages_included++;
    }

    if (passages_included == 0) {
        free(builder.data);
        return NULL;
    }

    if (string_builder_append(&builder, "Question: ") != 0 ||
        string_builder_append(&builder, query_text) != 0 ||
        string_builder_append(&builder, "\n\nAnswer:") != 0) {
        goto fail;
    }

    return builder.data;

fail:
    free(builder.data);
    return NULL;
}

char *generation_generate_answer(const char *query_text, PgStore *store,
                                  const BM25ResultSet *results) {
    char *prompt = generation_build_prompt(query_text, store, results);
    if (prompt == NULL) {
        return NULL;
    }

    char *answer = local_llm_chat_completion(prompt);
    free(prompt);
    return answer;
}
