#include "corpus_summary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "local_llm_client.h"
#include "prompts.h"
#include "string_builder.h"

/* Held out of the sampling budget for the instruction block, the model's
 * own output, and the chat template's markup. Generous on purpose: an
 * oversized prompt makes local_llm_chat_completion_multi() fail outright
 * rather than degrade, and the whole point of this module is to be the
 * fast path. */
#define CORPUS_SUMMARY_RESERVED_TOKENS 3000

/* Same conservative bytes-per-token estimate generation.c uses for
 * byte-length truncation -- real English averages closer to 4, so
 * undershooting the budget is the safe direction. Excerpt boundaries are
 * chosen by byte offset, not token offset, because slicing text needs
 * byte positions and an exact token count per candidate slice would mean
 * tokenizing the whole corpus just to decide where to cut. */
#define CORPUS_SUMMARY_BYTES_PER_TOKEN 3

/* How many excerpts to take from a document too large to include whole:
 * the head, plus this many more spread evenly across the remainder. The
 * head is weighted because manuals and reports front-load their scope,
 * while the spread exists so a document that changes subject halfway
 * through is not summarized entirely from its introduction. */
#define CORPUS_SUMMARY_EXCERPTS_PER_DOC 3

/* Below this, sampling a document is pointless -- include it whole. */
#define CORPUS_SUMMARY_MIN_EXCERPT_BYTES 400

/* Appends up to `budget_bytes` of `text`, as either the whole thing or
 * CORPUS_SUMMARY_EXCERPTS_PER_DOC evenly spaced excerpts joined by an
 * elision marker. Returns 0 on success, -1 on allocation failure.
 *
 * Excerpts are cut on byte boundaries, which can split a UTF-8 sequence
 * or a word. That is acceptable for text being fed to a tokenizer for
 * gist extraction -- a mid-character cut costs one garbled token at each
 * seam, and never reaches the user, who only ever sees the generated
 * summary. */
static int append_sampled_text(StringBuilder *builder, const char *text, size_t budget_bytes) {
    size_t len = strlen(text);
    if (len <= budget_bytes) {
        return string_builder_append(builder, text);
    }

    size_t per_excerpt = budget_bytes / CORPUS_SUMMARY_EXCERPTS_PER_DOC;
    if (per_excerpt < CORPUS_SUMMARY_MIN_EXCERPT_BYTES) {
        /* Budget too small to spread meaningfully -- take one head slice
         * rather than several fragments too short to carry a topic. */
        per_excerpt = budget_bytes;
    }

    size_t taken = 0;
    for (size_t i = 0; i < CORPUS_SUMMARY_EXCERPTS_PER_DOC && taken < budget_bytes; i++) {
        /* Excerpt i starts proportionally through the document: 0, then
         * 1/3, then 2/3 for the default of 3. */
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
    /* Split evenly across documents so one enormous document can't crowd
     * the others out of the sample entirely -- the opposite of
     * generation.c's whole-document policy, and deliberately so: a
     * summary that describes only the first document misdescribes the
     * group. */
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
    /* The group's current document count, needed both as the staleness key
     * and to know whether there is anything to summarize at all. */
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
        /* Stale: the group has gained or lost documents since this was
         * written, so it describes a collection that no longer exists. */
        free(cached);
    }

    char *summary = corpus_summary_build(store);
    if (summary == NULL) {
        return NULL;
    }

    /* A cache write failing is not a reason to fail the answer -- the
     * summary in hand is still correct, it just won't be reused. */
    if (pg_store_set_corpus_summary(store, corpus_id, summary, (int)doc_count) != 0) {
        fprintf(stderr, "corpus_summary_get_or_build: could not cache summary for corpus %lld\n",
                (long long)corpus_id);
    }
    return summary;
}
