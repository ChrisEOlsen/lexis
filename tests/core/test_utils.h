/* Minimal test harness: TEST_ASSERT counts failures; test_summary() returns exit code. */

#ifndef LEXIS_TEST_UTILS_H
#define LEXIS_TEST_UTILS_H

#include <stdio.h>
#include <stdlib.h>

/* Throwaway lexis_test DB; override via LEXIS_TEST_CONNINFO (see docs/building.md). */
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
