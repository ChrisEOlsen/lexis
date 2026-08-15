/*
 * Implementation of the optional embedding reranker.
 * See include/reranker.h for the module's role.
 */

#define _POSIX_C_SOURCE 200809L

#include "reranker.h"

#include <llama.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* bge-v1.5 models want this prefix on the QUERY side only (passages are
 * embedded bare) -- it is part of how the model was trained, not
 * decoration. */
#define RERANKER_QUERY_PREFIX "Represent this sentence for searching relevant passages: "

#define RERANKER_N_CTX 512
#define RERANKER_RRF_K 60.0

static struct llama_model *g_model = NULL;
static struct llama_context *g_ctx = NULL;
static const struct llama_vocab *g_vocab = NULL;
static int g_n_embd = 0;

int reranker_available(void) {
    return g_ctx != NULL;
}

int reranker_init(const char *model_path) {
    if (g_ctx != NULL) {
        return 0;
    }

    /* Idempotent; needed because this module can be the FIRST llama.cpp
     * user in a process (eval --no-llm-expansion never loads the chat
     * model, and that path is exactly how the reranker gets measured). */
    llama_backend_init();

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 999;
    g_model = llama_model_load_from_file(model_path, mparams);
    if (g_model == NULL) {
        fprintf(stderr, "reranker_init: failed to load model from %s\n", model_path);
        return -1;
    }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = RERANKER_N_CTX;
    cparams.n_batch = RERANKER_N_CTX;
    cparams.n_ubatch = RERANKER_N_CTX;
    cparams.embeddings = true;
    /* bge is a BERT-family model: sentence embedding = the CLS token's. */
    cparams.pooling_type = LLAMA_POOLING_TYPE_CLS;
    g_ctx = llama_init_from_model(g_model, cparams);
    if (g_ctx == NULL) {
        fprintf(stderr, "reranker_init: failed to create context\n");
        llama_model_free(g_model);
        g_model = NULL;
        return -1;
    }
    g_vocab = llama_model_get_vocab(g_model);
    g_n_embd = llama_model_n_embd(g_model);
    return 0;
}

void reranker_cleanup(void) {
    if (g_ctx != NULL) {
        llama_free(g_ctx);
        g_ctx = NULL;
    }
    if (g_model != NULL) {
        llama_model_free(g_model);
        g_model = NULL;
    }
    g_vocab = NULL;
    g_n_embd = 0;
}

/* Embeds one text into `out` (g_n_embd floats, L2-normalized so cosine
 * is a plain dot product). 0 on success. */
static int embed_text(const char *text, float *out) {
    llama_token tokens[RERANKER_N_CTX];
    int n_tokens = llama_tokenize(g_vocab, text, (int32_t)strlen(text), tokens, RERANKER_N_CTX,
                                  /*add_special=*/true, /*parse_special=*/false);
    if (n_tokens < 0) {
        /* Text longer than the window: llama_tokenize reports the needed
         * count as negative. Re-tokenize truncated -- embedding the
         * passage's head is fine for similarity purposes. */
        n_tokens = llama_tokenize(g_vocab, text, (int32_t)strlen(text), tokens, RERANKER_N_CTX,
                                  true, false);
        if (n_tokens < 0) {
            n_tokens = RERANKER_N_CTX;
        }
    }
    if (n_tokens <= 0) {
        return -1;
    }

    llama_memory_clear(llama_get_memory(g_ctx), true);
    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    int rc = llama_model_has_encoder(g_model) ? llama_encode(g_ctx, batch)
                                              : llama_decode(g_ctx, batch);
    if (rc != 0) {
        return -1;
    }

    const float *emb = llama_get_embeddings_seq(g_ctx, 0);
    if (emb == NULL) {
        return -1;
    }
    double norm = 0.0;
    for (int i = 0; i < g_n_embd; i++) {
        norm += (double)emb[i] * (double)emb[i];
    }
    norm = sqrt(norm);
    if (norm <= 0.0) {
        return -1;
    }
    for (int i = 0; i < g_n_embd; i++) {
        out[i] = (float)((double)emb[i] / norm);
    }
    return 0;
}

typedef struct {
    size_t index;   /* position in the original (BM25-ordered) items[] */
    double cosine;
} RerankEntry;

static int compare_cosine_desc(const void *a, const void *b) {
    double ca = ((const RerankEntry *)a)->cosine;
    double cb = ((const RerankEntry *)b)->cosine;
    if (ca < cb) return 1;
    if (ca > cb) return -1;
    return 0;
}

static int compare_score_desc(const void *a, const void *b) {
    const BM25ScoredPassage *pa = (const BM25ScoredPassage *)a;
    const BM25ScoredPassage *pb = (const BM25ScoredPassage *)b;
    if (pa->score < pb->score) return 1;
    if (pa->score > pb->score) return -1;
    if (pa->passage_id < pb->passage_id) return -1;
    if (pa->passage_id > pb->passage_id) return 1;
    return 0;
}

int reranker_rescore(PgStore *store, const char *query_text, BM25ResultSet *set) {
    if (!reranker_available() || set == NULL || set->count < 2) {
        return 0; /* nothing to reorder */
    }

    float *query_emb = malloc((size_t)g_n_embd * sizeof(float));
    float *passage_emb = malloc((size_t)g_n_embd * sizeof(float));
    RerankEntry *entries = malloc(set->count * sizeof(RerankEntry));
    size_t prefix_len = strlen(RERANKER_QUERY_PREFIX);
    char *prefixed = malloc(prefix_len + strlen(query_text) + 1);
    if (query_emb == NULL || passage_emb == NULL || entries == NULL || prefixed == NULL) {
        goto fail;
    }
    memcpy(prefixed, RERANKER_QUERY_PREFIX, prefix_len);
    strcpy(prefixed + prefix_len, query_text);

    if (embed_text(prefixed, query_emb) != 0) {
        goto fail;
    }

    for (size_t i = 0; i < set->count; i++) {
        PgStorePassage *passage = pg_store_get_passage(store, set->items[i].passage_id);
        if (passage == NULL) {
            /* Unfetchable passage: worst cosine, keeps its BM25 standing
             * only through the fusion's BM25 half. */
            entries[i].index = i;
            entries[i].cosine = -1.0;
            continue;
        }
        double cosine = -1.0;
        if (embed_text(passage->text, passage_emb) == 0) {
            double dot = 0.0;
            for (int d = 0; d < g_n_embd; d++) {
                dot += (double)query_emb[d] * (double)passage_emb[d];
            }
            cosine = dot;
        }
        pg_store_passage_free(passage);
        entries[i].index = i;
        entries[i].cosine = cosine;
    }

    /* Reciprocal-rank fusion: rank-based, so BM25's unbounded scores and
     * cosine's [-1,1] never need to share a scale. items[] is already in
     * BM25 rank order, so bm25_rank(i) == i. */
    qsort(entries, set->count, sizeof(RerankEntry), compare_cosine_desc);
    double *fused = malloc(set->count * sizeof(double));
    if (fused == NULL) {
        goto fail;
    }
    for (size_t cos_rank = 0; cos_rank < set->count; cos_rank++) {
        size_t i = entries[cos_rank].index;
        fused[i] = 1.0 / (RERANKER_RRF_K + (double)i + 1.0) +
                   1.0 / (RERANKER_RRF_K + (double)cos_rank + 1.0);
    }
    for (size_t i = 0; i < set->count; i++) {
        set->items[i].score = fused[i];
    }
    free(fused);
    qsort(set->items, set->count, sizeof(BM25ScoredPassage), compare_score_desc);

    free(prefixed);
    free(entries);
    free(passage_emb);
    free(query_emb);
    return 0;

fail:
    free(prefixed);
    free(entries);
    free(passage_emb);
    free(query_emb);
    return -1;
}
