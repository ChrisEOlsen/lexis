#include "corpus_summary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "local_llm_client.h"
#include "prompts.h"
#include "string_builder.h"

/* Headroom for instructions, output, and template markup; oversized prompts fail outright. */
#define CORPUS_SUMMARY_RESERVED_TOKENS 3000

/* Conservative 3 bytes/token (real ~4, same as generation.c); byte offsets avoid tokenizing to cut. */
#define CORPUS_SUMMARY_BYTES_PER_TOKEN 3

/* Excerpts per oversized doc: head plus even spread, so topic shifts aren't missed. */
#define CORPUS_SUMMARY_EXCERPTS_PER_DOC 3

/* Below this, sampling a document is pointless -- include it whole. */
#define CORPUS_SUMMARY_MIN_EXCERPT_BYTES 400

/* Append up to budget_bytes as whole text or spaced excerpts (0 ok, -1 alloc fail).
 * Byte cuts may split UTF-8; acceptable for gist input, never shown to user. */
static int append_sampled_text(StringBuilder *builder, const char *text, size_t budget_bytes) {
    size_t len = strlen(text);
    if (len <= budget_bytes) {
        return string_builder_append(builder, text);
    }

    size_t per_excerpt = budget_bytes / CORPUS_SUMMARY_EXCERPTS_PER_DOC;
    if (per_excerpt < CORPUS_SUMMARY_MIN_EXCERPT_BYTES) {
        /* Too small to spread: one head slice beats tiny fragments. */
        per_excerpt = budget_bytes;
    }

    size_t taken = 0;
    for (size_t i = 0; i < CORPUS_SUMMARY_EXCERPTS_PER_DOC && taken < budget_bytes; i++) {
        size_t start = (len / CORPUS_SUMMARY_EXCERPTS_PER_DOC) * i;
        size_t remaining_budget = budget_bytes - taken;
        size_t take = per_excerpt < remaining_budget ? per_excerpt : remaining_budget;
        if (start + take > len) {
            take = len - start;
        }
        if (take == 0) {
            break;
        }

        if (i > 0 && string_builder_append(builder, "\n[...]\n") != 0) {
            return -1;
        }

        char *slice = malloc(take + 1);
        if (slice == NULL) {
            return -1;
        }
        memcpy(slice, text + start, take);
        slice[take] = '\0';
        int failed = string_builder_append(builder, slice);
        free(slice);
        if (failed != 0) {
            return -1;
        }
        taken += take;
    }
    return 0;
}

char *corpus_summary_build(PgStore *store) {
    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    if (docs == NULL) {
        return NULL;
    }
    if (doc_count == 0) {
        pg_store_documents_free(docs, doc_count);
        return NULL;
    }

    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder, LEXIS_PROMPT_BUILD_SUMMARY_HEAD) != 0) {
        goto fail;
    }

    int budget_tokens = LOCAL_LLM_N_CTX - CORPUS_SUMMARY_RESERVED_TOKENS;
    if (budget_tokens < 0) {
        budget_tokens = 0;
    }
    /* Split evenly per doc so one huge doc can't crowd out the rest. */
    size_t budget_bytes_total = (size_t)budget_tokens * CORPUS_SUMMARY_BYTES_PER_TOKEN;
    size_t budget_per_doc = budget_bytes_total / doc_count;

    for (size_t i = 0; i < doc_count; i++) {
        if (string_builder_append(&builder, "[") != 0 ||
            string_builder_append(&builder, docs[i].document_name) != 0 ||
            string_builder_append(&builder, "]\n") != 0) {
            goto fail;
        }
        if (docs[i].text != NULL && append_sampled_text(&builder, docs[i].text, budget_per_doc) != 0) {
            goto fail;
        }
        if (string_builder_append(&builder, "\n\n") != 0) {
            goto fail;
        }
    }

    pg_store_documents_free(docs, doc_count);

    LocalLlmTurn turn = {.role = "user", .content = builder.data};
    char *summary = local_llm_chat_completion_multi(&turn, 1, LEXIS_PREFILL_NO_THINK);
    free(builder.data);
    return summary;

fail:
    free(builder.data);
    pg_store_documents_free(docs, doc_count);
    return NULL;
}

char *corpus_summary_get_or_build(PgStore *store, int64_t corpus_id) {
    /* Doc count doubles as staleness key and empty check. */
    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    if (docs != NULL) {
        pg_store_documents_free(docs, doc_count);
    }
    if (doc_count == 0) {
        return NULL;
    }

    int cached_count = -1;
    char *cached = pg_store_get_corpus_summary(store, corpus_id, &cached_count);
    if (cached != NULL) {
        if (cached_count == (int)doc_count) {
            return cached;
        }
        /* Stale: doc set changed since this was written. */
        free(cached);
    }

    char *summary = corpus_summary_build(store);
    if (summary == NULL) {
        return NULL;
    }

    /* Cache-write failure still returns the correct summary, just uncached. */
    if (pg_store_set_corpus_summary(store, corpus_id, summary, (int)doc_count) != 0) {
        fprintf(stderr, "corpus_summary_get_or_build: could not cache summary for corpus %lld\n",
                (long long)corpus_id);
    }
    return summary;
}
