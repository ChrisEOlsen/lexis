/*
 * Implementation of runtime path resolution.
 * See include/paths.h for the module's role.
 */

#define _POSIX_C_SOURCE 200809L

#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_resource_dir = NULL;               /* NULL = working directory */
static const char *g_config_file = "config/lexis.conf";
static char *g_config_file_owned = NULL;

void lexis_paths_set(const char *resource_dir, const char *config_file) {
    if (resource_dir != NULL) {
        free(g_resource_dir);
        g_resource_dir = strdup(resource_dir);
    }
    if (config_file != NULL) {
        free(g_config_file_owned);
        g_config_file_owned = strdup(config_file);
        if (g_config_file_owned != NULL) {
            g_config_file = g_config_file_owned;
        }
    }
}

const char *lexis_paths_config_file(void) {
    return g_config_file;
}

char *lexis_paths_resource(const char *relative) {
    if (g_resource_dir == NULL) {
        return strdup(relative);
    }
    size_t len = strlen(g_resource_dir) + 1 + strlen(relative) + 1;
    char *joined = malloc(len);
    if (joined == NULL) {
        return NULL;
    }
    snprintf(joined, len, "%s/%s", g_resource_dir, relative);
    return joined;
}
