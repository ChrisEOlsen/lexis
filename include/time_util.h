/* Shared timing helper (header-only). Includers need _POSIX_C_SOURCE for timespec. */

#ifndef LEXIS_TIME_UTIL_H
#define LEXIS_TIME_UTIL_H

#include <time.h>

/* Whole milliseconds from start to end (monotonic-clock pair). */
static inline long lexis_elapsed_ms(struct timespec start, struct timespec end) {
    long seconds = end.tv_sec - start.tv_sec;
    long nanoseconds = end.tv_nsec - start.tv_nsec;
    return seconds * 1000 + nanoseconds / 1000000;
}

#endif /* LEXIS_TIME_UTIL_H */
