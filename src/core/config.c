/*
 * Implementation of the testing/production mode config reader.
 * See include/config.h for the module's role.
 */

/* See tokenizer.c for why this must come before any #include (strtok_r is
 * a POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deliberately not ingest_read_file(): a missing config file is a normal,
 * expected fallback path here (default to testing mode), not a real error
 * worth an stderr warning on every single run -- unlike ingest_read_file's
 * callers, where a missing file always means something actually failed. */
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

/* Walks `text` (destructively -- strtok_r/trim mutate it in place) and
 * returns the value of the last "key = value" line matching `key`, or
 * NULL if no such line exists. The returned pointer aliases `text`, so
 * it is only valid until `text` is freed. Last occurrence wins, matching
 * how the original mode parser behaved when a key was repeated. */
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
