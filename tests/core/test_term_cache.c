/*
 * Tests for src/core/term_cache.c -- the shared in-memory term cache.
 * Uses the real docker-compose Postgres instance (lexis_test database) --
 * `docker compose up -d` must be running for these to pass.
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "term_cache.h"
#include "pg_store.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CONNINFO "host=127.0.0.1 port=5433 dbname=lexis_test user=lexis password=lexis_dev_only"

static int int64_compare(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static PgStore *open_fresh_store(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    if (store == NULL) {
        return NULL;
    }
    PGresult *res = PQexec(store->conn, "TRUNCATE postings, terms, passages RESTART IDENTITY CASCADE;");
    PQclear(res);
    return store;
}

static void test_create_and_free_are_safe(void) {
    TermCache *cache = term_cache_create();
    TEST_ASSERT(cache != NULL, "expected term_cache_create to succeed");
    term_cache_free(cache);
    term_cache_free(NULL);
}

static void test_resolves_new_terms_and_stays_consistent(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is docker compose up?");
    TermCache *cache = term_cache_create();
    TEST_ASSERT(cache != NULL, "expected term_cache_create to succeed");

    const char *terms[2] = {"hypertension", "treatment"};
    TermCachePending *pending1 = term_cache_pending_create();
    int64_t *ids = term_cache_get_or_create_terms(cache, pending1, store, terms, 2);
    TEST_ASSERT(ids != NULL, "expected first resolve to succeed");
    TEST_ASSERT(ids[0] != ids[1], "expected distinct terms to get distinct ids");
    /* Simulates the document's transaction committing -- only after this
     * is it safe for other callers to see these terms via the shared
     * cache (see TermCachePending's doc comment). */
    TEST_ASSERT(term_cache_commit_pending(cache, pending1) == 0, "expected commit to succeed");

    /* Same call again, this time via the shared cache (fresh empty
     * pending -- nothing document-local to fall back on) -- whether
     * served from cache or re-resolved against Postgres, the ids must be
     * identical (a term always resolves to the same row). */
    TermCachePending *pending2 = term_cache_pending_create();
    int64_t *ids_again = term_cache_get_or_create_terms(cache, pending2, store, terms, 2);
    TEST_ASSERT(ids_again != NULL, "expected second resolve to succeed");
    term_cache_pending_free(pending2);
    TEST_ASSERT(ids_again[0] == ids[0], "expected \"hypertension\" to resolve to the same id both times");
    TEST_ASSERT(ids_again[1] == ids[1], "expected \"treatment\" to resolve to the same id both times");

    /* And it must actually match what's really in Postgres. */
    int64_t real_id = pg_store_lookup_term(store, "hypertension");
    TEST_ASSERT(real_id == ids[0], "expected the cached id to match the real terms table row");

    free(ids);
    free(ids_again);
    term_cache_free(cache);
    pg_store_close(store);
}

static void test_handles_duplicates_in_input(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");
    TermCache *cache = term_cache_create();
    TEST_ASSERT(cache != NULL, "expected term_cache_create to succeed");

    const char *terms[3] = {"alpha", "alpha", "beta"};
    TermCachePending *pending = term_cache_pending_create();
    int64_t *ids = term_cache_get_or_create_terms(cache, pending, store, terms, 3);
    TEST_ASSERT(ids != NULL, "expected resolve to succeed");
    TEST_ASSERT(ids[0] == ids[1], "expected both \"alpha\" occurrences to resolve to the same id");
    TEST_ASSERT(ids[0] != ids[2], "expected \"alpha\" and \"beta\" to resolve to different ids");

    term_cache_pending_free(pending);
    free(ids);
    term_cache_free(cache);
    pg_store_close(store);
}

static void test_preload_finds_existing_terms_without_reinserting(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    /* Create a term directly, bypassing the cache entirely -- simulates
     * vocabulary already known from a prior ingestion run. */
    int64_t original_id = pg_store_get_or_create_term(store, "preexisting");
    TEST_ASSERT(original_id != -1, "expected setup term creation to succeed");

    TermCache *cache = term_cache_create();
    TEST_ASSERT(cache != NULL, "expected term_cache_create to succeed");
    int preload_result = term_cache_preload(cache, store);
    TEST_ASSERT(preload_result == 0, "expected term_cache_preload to succeed");

    const char *terms[1] = {"preexisting"};
    TermCachePending *pending = term_cache_pending_create();
    int64_t *ids = term_cache_get_or_create_terms(cache, pending, store, terms, 1);
    TEST_ASSERT(ids != NULL, "expected resolve to succeed");
    TEST_ASSERT(ids[0] == original_id,
                "expected the preloaded term to resolve to its real, pre-existing id, not a new one");

    PGresult *res = PQexec(store->conn, "SELECT COUNT(*) FROM terms WHERE term = 'preexisting';");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1,
                "expected exactly 1 row for \"preexisting\" -- preload must not have caused a duplicate "
                "insert, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    term_cache_pending_free(pending);
    free(ids);
    term_cache_free(cache);
    pg_store_close(store);
}

static void test_grows_past_initial_capacity(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");
    TermCache *cache = term_cache_create();
    TEST_ASSERT(cache != NULL, "expected term_cache_create to succeed");

    /* Initial capacity is 1024 -- resolve enough distinct terms to force
     * several resizes, and verify every one survives with a correct,
     * distinct id (an old bug class: growing into the wrong slot
     * position, or losing entries during rehash, would show up as
     * missing/duplicate ids here). */
    const int term_count = 3000;
    char **terms_storage = malloc(sizeof(char *) * (size_t)term_count);
    const char **terms = malloc(sizeof(char *) * (size_t)term_count);
    TEST_ASSERT(terms_storage != NULL && terms != NULL, "expected setup allocation to succeed");
    for (int i = 0; i < term_count; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "growthterm%d", i);
        terms_storage[i] = strdup(buf);
        terms[i] = terms_storage[i];
    }

    TermCachePending *pending1 = term_cache_pending_create();
    int64_t *ids = term_cache_get_or_create_terms(cache, pending1, store, terms, (size_t)term_count);
    TEST_ASSERT(ids != NULL, "expected resolve to succeed");
    TEST_ASSERT(term_cache_commit_pending(cache, pending1) == 0, "expected commit to succeed");

    /* Distinctness via sort + adjacent-pair comparison (O(n log n)) --
     * checking every pair directly is O(n^2), which at 3000 terms is
     * ~4.5M assertions and needlessly slows down every `make check` run. */
    int64_t *sorted_ids = malloc(sizeof(int64_t) * (size_t)term_count);
    TEST_ASSERT(sorted_ids != NULL, "expected setup allocation to succeed");
    memcpy(sorted_ids, ids, sizeof(int64_t) * (size_t)term_count);
    qsort(sorted_ids, (size_t)term_count, sizeof(int64_t), int64_compare);
    for (int i = 1; i < term_count; i++) {
        TEST_ASSERT(sorted_ids[i] != sorted_ids[i - 1],
                    "expected all %d distinct terms to have distinct ids, found a duplicate", term_count);
    }
    free(sorted_ids);

    /* Re-resolving after growth must still hit the shared cache
     * correctly (fresh empty pending -- nothing document-local to fall
     * back on). */
    TermCachePending *pending2 = term_cache_pending_create();
    int64_t *ids_again = term_cache_get_or_create_terms(cache, pending2, store, terms, (size_t)term_count);
    TEST_ASSERT(ids_again != NULL, "expected re-resolve after growth to succeed");
    term_cache_pending_free(pending2);
    for (int i = 0; i < term_count; i++) {
        TEST_ASSERT(ids_again[i] == ids[i], "expected term %d to resolve consistently after growth", i);
    }

    free(ids);
    free(ids_again);
    for (int i = 0; i < term_count; i++) {
        free(terms_storage[i]);
    }
    free(terms_storage);
    free((void *)terms);
    term_cache_free(cache);
    pg_store_close(store);
}

/* Regression test for a real, verified bug: a term newly resolved inside
 * a transaction that later rolls back must never end up in the shared
 * cache. The first version of this module wrote straight into the
 * shared cache the moment pg_store_get_or_create_terms() returned an
 * id -- but that INSERT only really happened if the surrounding
 * transaction commits. Verified directly against the full 8.84M-passage
 * MS MARCO ingest: a rolled-back transaction (e.g. from a deadlock on a
 * *later* chunk in the same document) left the cache claiming a
 * non-existent term_id existed, and every subsequent document using that
 * term failed with a real Postgres foreign-key violation
 * (postings_term_id_fkey) for the rest of the run. */
static void test_pending_discarded_on_rollback_does_not_poison_cache(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");
    TermCache *cache = term_cache_create();
    TEST_ASSERT(cache != NULL, "expected term_cache_create to succeed");

    TEST_ASSERT(pg_store_begin_transaction(store) == 0, "expected transaction begin to succeed");

    const char *terms[1] = {"rolledback"};
    TermCachePending *pending = term_cache_pending_create();
    int64_t *ids = term_cache_get_or_create_terms(cache, pending, store, terms, 1);
    TEST_ASSERT(ids != NULL, "expected resolve to succeed");
    int64_t phantom_id = ids[0];
    free(ids);

    /* Simulates a failure later in the same document -- the whole
     * transaction rolls back, so this term never actually persists. */
    TEST_ASSERT(pg_store_rollback_transaction(store) == 0, "expected rollback to succeed");
    term_cache_pending_free(pending);

    /* The shared cache must NOT have this term cached -- resolving it
     * again (fresh pending, nothing document-local to fall back on)
     * must go back to Postgres and get a genuinely new row, not the
     * phantom id from the rolled-back transaction. */
    TermCachePending *pending2 = term_cache_pending_create();
    int64_t *ids2 = term_cache_get_or_create_terms(cache, pending2, store, terms, 1);
    TEST_ASSERT(ids2 != NULL, "expected re-resolve to succeed");
    TEST_ASSERT(ids2[0] != phantom_id,
                "expected a fresh, real id -- the shared cache must not have kept the phantom id from "
                "the rolled-back transaction");
    TEST_ASSERT(term_cache_commit_pending(cache, pending2) == 0, "expected commit to succeed");

    /* And that id must actually exist in Postgres -- proving a posting
     * referencing it won't hit the foreign-key violation that originally
     * surfaced this bug. */
    int64_t real_id = pg_store_lookup_term(store, "rolledback");
    TEST_ASSERT(real_id == ids2[0], "expected the resolved id to be a real, persisted terms row");

    free(ids2);
    term_cache_free(cache);
    pg_store_close(store);
}

int main(void) {
    test_create_and_free_are_safe();
    test_resolves_new_terms_and_stays_consistent();
    test_handles_duplicates_in_input();
    test_preload_finds_existing_terms_without_reinserting();
    test_grows_past_initial_capacity();
    test_pending_discarded_on_rollback_does_not_poison_cache();
    return test_summary();
}
