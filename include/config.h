/* Centralized runtime settings (config/lexis.conf): one getter per key. */

#ifndef LEXIS_CONFIG_H
#define LEXIS_CONFIG_H

#include <stddef.h>

/* TESTING logs every query via query_log; PRODUCTION skips logging entirely. */
typedef enum {
    LEXIS_MODE_TESTING,
    LEXIS_MODE_PRODUCTION
} LexisMode;

/* Read "mode = testing|production" from path. Missing file/line defaults to TESTING. */
LexisMode config_load_mode(const char *path);

/* model_path fallback. Keep scripts/download_model.sh's fallback in sync when this changes. */
#define LEXIS_DEFAULT_MODEL_PATH "data/models/gemma-4-E4B-it-Q4_K_M.gguf"

/* Read "model_path" from path, else LEXIS_DEFAULT_MODEL_PATH. Caller frees; NULL on alloc failure. */
char *config_load_model_path(const char *path);

/* Default config location, relative to the project root working directory. */
#define LEXIS_CONFIG_PATH_DEFAULT "config/lexis.conf"

/* Read "thinking = on|off" (answer generation only). Missing/unrecognized defaults to 1 (on). */
int config_load_thinking(const char *path);

/* Read "reranker_model_path". Caller frees; NULL when missing = reranker off (the default). */
char *config_load_reranker_model_path(const char *path);

/* Read "db_conninfo". Caller frees; NULL when missing. No fallback: it embeds a password. */
char *config_load_db_conninfo(const char *path);

/* Ingest fallbacks (bulk-ingest chunking/workers). LEXIS_* env, when set, wins over these keys. */
#define LEXIS_DEFAULT_CHUNK_SIZE 200
#define LEXIS_DEFAULT_CHUNK_OVERLAP 40
#define LEXIS_DEFAULT_INGEST_THREADS 6

/* Read "chunk_size". Missing/invalid (not > 0) defaults to LEXIS_DEFAULT_CHUNK_SIZE. */
size_t config_load_chunk_size(const char *path);

/* Read "chunk_overlap". Missing/invalid (negative) defaults to LEXIS_DEFAULT_CHUNK_OVERLAP. */
size_t config_load_chunk_overlap(const char *path);

/* Read "ingest_threads". Missing/invalid (not > 0) defaults to LEXIS_DEFAULT_INGEST_THREADS. */
int config_load_ingest_threads(const char *path);

/* Retrieval fallbacks live in bm25.h (LEXIS_SEARCH_*, BM25_DEFAULT_*); these keys override them. */

/* Read "candidate_ceiling". Missing/invalid (not > 0) defaults to LEXIS_SEARCH_CANDIDATE_CEILING. */
size_t config_load_candidate_ceiling(const char *path);

/* Read "max_passages". Missing/invalid (not > 0) defaults to LEXIS_SEARCH_MAX_PASSAGES. */
size_t config_load_max_passages(const char *path);

/* Read "token_budget". Missing/invalid (not > 0) defaults to LEXIS_SEARCH_TOKEN_BUDGET. */
int config_load_token_budget(const char *path);

/* Read "score_floor_ratio". Missing/invalid (negative) defaults to LEXIS_SEARCH_SCORE_FLOOR_RATIO. */
double config_load_score_floor_ratio(const char *path);

/* Read "bm25_k1". Missing/invalid (not > 0) defaults to BM25_DEFAULT_K1. */
double config_load_bm25_k1(const char *path);

/* Read "bm25_b". Missing/invalid (negative) defaults to BM25_DEFAULT_B. */
double config_load_bm25_b(const char *path);

#endif /* LEXIS_CONFIG_H */
