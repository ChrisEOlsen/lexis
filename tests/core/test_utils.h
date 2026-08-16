/*
 * Minimal test harness shared by every test file under tests/core. No external
 * dependencies: TEST_ASSERT reports and keeps going on failure (rather
 * than aborting), tracking a pass/fail count. test_summary() prints the
 * result and returns a value meant to be used as the process exit code
 * (0 = all passed, 1 = at least one failure) — wireable into `make check`.
 */

#ifndef LEXIS_TEST_UTILS_H
#define LEXIS_TEST_UTILS_H

#include <stdio.h>
#include <stdlib.h>

/* The lexis_test database connection. The default below is the
 * conventional local dev setup documented in docs/building.md -- a
 * localhost-only throwaway database. A machine using its own password
 * overrides via the LEXIS_TEST_CONNINFO environment variable; nothing
 * here is a credential for any real data. */
static inline const char *test_conninfo(void) {
    const char *env = getenv("LEXIS_TEST_CONNINFO");
    return (env != NULL && env[0] != '\0')
               ? env
               : "host=127.0.0.1 port=5434 dbname=lexis_test user=lexis password=lexis_dev_only";
}
#include <string.h>

static int test_utils_run = 0;
static int test_utils_failed = 0;

#define TEST_ASSERT(cond, ...) \
    do { \
        test_utils_run++; \
        if (!(cond)) { \
            test_utils_failed++; \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

#define TEST_ASSERT_STR_EQ(actual, expected) \
    TEST_ASSERT(strcmp((actual), (expected)) == 0, \
                "expected \"%s\", got \"%s\"", (expected), (actual))

static int test_summary(void) {
    printf("%d passed, %d failed\n", test_utils_run - test_utils_failed, test_utils_failed);
    return test_utils_failed == 0 ? 0 : 1;
}

#endif /* LEXIS_TEST_UTILS_H */
