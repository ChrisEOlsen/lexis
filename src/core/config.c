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

LexisMode config_load_mode(const char *path) {
    char *text = read_file_quietly(path);
    if (text == NULL) {
        return LEXIS_MODE_TESTING;
    }

    LexisMode mode = LEXIS_MODE_TESTING;
    char *saveptr;
    char *line = strtok_r(text, "\n", &saveptr);
    while (line != NULL) {
        char *trimmed = trim(line);
        if (trimmed[0] != '\0' && trimmed[0] != '#') {
            char *equals = strchr(trimmed, '=');
            if (equals != NULL) {
                *equals = '\0';
                char *key = trim(trimmed);
                char *value = trim(equals + 1);
                if (strcmp(key, "mode") == 0) {
                    mode = (strcmp(value, "production") == 0) ? LEXIS_MODE_PRODUCTION
                                                               : LEXIS_MODE_TESTING;
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(text);
    return mode;
}
