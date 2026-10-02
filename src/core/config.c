/* Testing/production mode config reader; see config.h. */

/* Before any #include: exposes strtok_r under strict -std=c11 (see tokenizer.c). */
#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include "bm25.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Missing config is a normal fallback, not an error; hence no stderr warning. */
static char *read_file_quietly(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size == -1) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(fp);
        return NULL;
    }

    long bytes_read = (long)fread(buffer, 1, (size_t)size, fp);
    fclose(fp);
    if (bytes_read != size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    return buffer;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

/* Last "key = value" match for key (last wins); mutates text, result aliases it. */
static const char *find_last_value(char *text, const char *key) {
    const char *found = NULL;
    char *saveptr;
    char *line = strtok_r(text, "\n", &saveptr);
    while (line != NULL) {
        char *trimmed = trim(line);
        if (trimmed[0] != '\0' && trimmed[0] != '#') {
            char *equals = strchr(trimmed, '=');
            if (equals != NULL) {
                *equals = '\0';
                char *candidate_key = trim(trimmed);
                char *value = trim(equals + 1);
                if (strcmp(candidate_key, key) == 0) {
                    found = value;
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return found;
}

LexisMode config_load_mode(const char *path) {
    char *text = read_file_quietly(path);
    if (text == NULL) {
        return LEXIS_MODE_TESTING;
    }

    const char *value = find_last_value(text, "mode");
    LexisMode mode = (value != NULL && strcmp(value, "production") == 0)
                         ? LEXIS_MODE_PRODUCTION
                         : LEXIS_MODE_TESTING;

    free(text);
    return mode;
}

char *config_load_reranker_model_path(const char *path) {
    char *text = read_file_quietly(path);
    if (text == NULL) {
        return NULL;
    }
    const char *value = find_last_value(text, "reranker_model_path");
    char *result = (value != NULL && value[0] != '\0') ? strdup(value) : NULL;
    free(text);
    return result;
}

char *config_load_db_conninfo(const char *path) {
    char *text = read_file_quietly(path);
    if (text == NULL) {
        return NULL;
    }
    const char *value = find_last_value(text, "db_conninfo");
    char *result = (value != NULL && value[0] != '\0') ? strdup(value) : NULL;
    free(text);
    return result;
}

int config_load_thinking(const char *path) {
    char *text = read_file_quietly(path);
    if (text == NULL) {
        return 1;
    }

    const char *value = find_last_value(text, "thinking");
    int thinking = (value == NULL || strcmp(value, "off") != 0) ? 1 : 0;

    free(text);
    return thinking;
}

char *config_load_model_path(const char *path) {
    char *text = read_file_quietly(path);
    char *result = NULL;

    if (text != NULL) {
        const char *value = find_last_value(text, "model_path");
        if (value != NULL && value[0] != '\0') {
            result = strdup(value);
            if (result == NULL) {
                free(text);
                return NULL; /* allocation failure, not "use the default" */
            }
        }
        free(text);
    }

    if (result == NULL) {
        result = strdup(LEXIS_DEFAULT_MODEL_PATH);
    }
    return result;
}

/* Parsed long for key, or fallback when missing/unparseable (partial parses rejected). */
static long load_long_or(const char *path, const char *key, long fallback) {
    char *text = read_file_quietly(path);
    if (text == NULL) {
        return fallback;
    }
    long result = fallback;
    const char *value = find_last_value(text, key);
    if (value != NULL && value[0] != '\0') {
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end != value && *end == '\0') {
            result = parsed;
        }
    }
    free(text);
    return result;
}

/* Parsed double for key, or fallback when missing/unparseable (partial parses rejected). */
static double load_double_or(const char *path, const char *key, double fallback) {
    char *text = read_file_quietly(path);
    if (text == NULL) {
        return fallback;
    }
    double result = fallback;
    const char *value = find_last_value(text, key);
    if (value != NULL && value[0] != '\0') {
        char *end = NULL;
        double parsed = strtod(value, &end);
        if (end != value && *end == '\0') {
            result = parsed;
        }
    }
    free(text);
    return result;
}

size_t config_load_chunk_size(const char *path) {
    long parsed = load_long_or(path, "chunk_size", LEXIS_DEFAULT_CHUNK_SIZE);
    return parsed > 0 ? (size_t)parsed : LEXIS_DEFAULT_CHUNK_SIZE;
}

size_t config_load_chunk_overlap(const char *path) {
    long parsed = load_long_or(path, "chunk_overlap", LEXIS_DEFAULT_CHUNK_OVERLAP);
    return parsed >= 0 ? (size_t)parsed : LEXIS_DEFAULT_CHUNK_OVERLAP;
}

int config_load_ingest_threads(const char *path) {
    long parsed = load_long_or(path, "ingest_threads", LEXIS_DEFAULT_INGEST_THREADS);
    return parsed > 0 ? (int)parsed : LEXIS_DEFAULT_INGEST_THREADS;
}

size_t config_load_candidate_ceiling(const char *path) {
    long parsed = load_long_or(path, "candidate_ceiling", LEXIS_SEARCH_CANDIDATE_CEILING);
    return parsed > 0 ? (size_t)parsed : LEXIS_SEARCH_CANDIDATE_CEILING;
}

size_t config_load_max_passages(const char *path) {
    long parsed = load_long_or(path, "max_passages", LEXIS_SEARCH_MAX_PASSAGES);
    return parsed > 0 ? (size_t)parsed : LEXIS_SEARCH_MAX_PASSAGES;
}

int config_load_token_budget(const char *path) {
    long parsed = load_long_or(path, "token_budget", LEXIS_SEARCH_TOKEN_BUDGET);
    return parsed > 0 ? (int)parsed : LEXIS_SEARCH_TOKEN_BUDGET;
}

double config_load_score_floor_ratio(const char *path) {
    double parsed = load_double_or(path, "score_floor_ratio", LEXIS_SEARCH_SCORE_FLOOR_RATIO);
    return parsed >= 0.0 ? parsed : LEXIS_SEARCH_SCORE_FLOOR_RATIO;
}

double config_load_bm25_k1(const char *path) {
    double parsed = load_double_or(path, "bm25_k1", BM25_DEFAULT_K1);
    return parsed > 0.0 ? parsed : BM25_DEFAULT_K1;
}

double config_load_bm25_b(const char *path) {
    double parsed = load_double_or(path, "bm25_b", BM25_DEFAULT_B);
    return parsed >= 0.0 ? parsed : BM25_DEFAULT_B;
}
