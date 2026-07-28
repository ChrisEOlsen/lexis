/*
 * Centralized testing/production mode switch (config/lexis.conf). Lets the
 * whole pipeline's behavior be toggled from one place rather than scattered
 * per-feature flags -- currently gates query_log.c's pipeline observability
 * logging, since that logging has a real (if small) per-query cost that
 * production traffic shouldn't have to pay. See LIMITATIONS.md for what
 * else config/lexis.conf.example still doesn't wire up (model, chunk size,
 * etc.) -- this module deliberately only parses `mode`, not a general
 * key-value config system, since that's the only setting anything needs
 * right now.
 */

#ifndef LEXIS_CONFIG_H
#define LEXIS_CONFIG_H

/* Testing: query_log.c's full pipeline observability logging is active on
 * every query. Production: logging is skipped entirely -- no queries/
 * query_formulation_runs/search_runs/search_results/generation_runs
 * writes, avoiding their latency cost. */
typedef enum {
    LEXIS_MODE_TESTING,
    LEXIS_MODE_PRODUCTION
} LexisMode;

/* Reads a "mode = testing|production" line from the config file at `path`
 * (see config/lexis.conf.example for the format). A missing config file is
 * a normal, expected state (not every checkout has copied the example into
 * a real config/lexis.conf yet) and quietly defaults to LEXIS_MODE_TESTING
 * -- preserving today's always-on logging behavior. Production is an
 * explicit opt-in via the config file, never a silent default. */
LexisMode config_load_mode(const char *path);

#endif /* LEXIS_CONFIG_H */
