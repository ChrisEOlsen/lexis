/*
 * Implementation of the shared in-memory term cache.
 * See include/term_cache.h for the module's role and why it exists.
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "term_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Open-addressing (linear probing) hash table mapping term -> id.
 * slots[i].term == NULL marks an empty slot. */
typedef struct {
    char *term;
    int64_t id;
} TermCacheSlot;

struct TermCache {
    TermCacheSlot *slots;
    size_t capacity;
    size_t count;
    pthread_mutex_t mutex;
};

/* FNV-1a -- simple, well-distributed string hash. */
static uint64_t term_cache_hash(const char *term) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *)term; *p != '\0'; p++) {
        hash ^= (uint64_t)*p;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/* Finds term's slot via linear probing -- either an existing occupied
 * slot (already present) or the first empty slot found (not present,
 * ready for insertion there). Assumes the table is never allowed to
 * fill completely (see the load-factor check before insertion), so this
 * always terminates. */
static size_t term_cache_find_slot(TermCacheSlot *slots, size_t capacity, const char *term) {
    size_t slot = (size_t)(term_cache_hash(term) % capacity);
    while (slots[slot].term != NULL && strcmp(slots[slot].term, term) != 0) {
        slot = (slot + 1) % capacity;
    }
    return slot;
}

TermCache *term_cache_create(void) {
    TermCache *cache = malloc(sizeof(TermCache));
    if (cache == NULL) {
        return NULL;
    }
    cache->capacity = 1024;
    cache->slots = calloc(cache->capacity, sizeof(TermCacheSlot));
    if (cache->slots == NULL) {
        free(cache);
        return NULL;
    }
    cache->count = 0;
    pthread_mutex_init(&cache->mutex, NULL);
    return cache;
}

void term_cache_free(TermCache *cache) {
    if (cache == NULL) {
        return;
    }
    for (size_t i = 0; i < cache->capacity; i++) {
        free(cache->slots[i].term);
    }
    free(cache->slots);
    pthread_mutex_destroy(&cache->mutex);
    free(cache);
}

/* Must be called with cache->mutex already held. Doubles the table and
 * re-inserts every occupied slot -- probe sequences depend on capacity,
 * so old positions can't just be copied over. Returns 0 on success, -1
 * on allocation failure (the table is left unchanged). */
static int term_cache_grow_locked(TermCache *cache) {
    size_t new_capacity = cache->capacity * 2;
    TermCacheSlot *new_slots = calloc(new_capacity, sizeof(TermCacheSlot));
    if (new_slots == NULL) {
        return -1;
    }
    for (size_t i = 0; i < cache->capacity; i++) {
        if (cache->slots[i].term == NULL) {
            continue;
        }
        size_t slot = term_cache_find_slot(new_slots, new_capacity, cache->slots[i].term);
        new_slots[slot] = cache->slots[i];
    }
    free(cache->slots);
    cache->slots = new_slots;
    cache->capacity = new_capacity;
    return 0;
}

/* Must be called with cache->mutex already held. Inserts (term, id),
 * taking a fresh strdup()'d copy of `term` -- the caller keeps ownership
 * of the string it passed in. If `term` is already present, this is a
 * harmless overwrite (a term always resolves to the same id no matter
 * which thread discovers it first, since Postgres's own uniqueness
 * constraint guarantees that). Returns 0 on success, -1 on allocation
 * failure. */
static int term_cache_insert_locked(TermCache *cache, const char *term, int64_t id) {
    if ((cache->count + 1) * 10 >= cache->capacity * 7) {
        if (term_cache_grow_locked(cache) != 0) {
            return -1;
        }
    }
    size_t slot = term_cache_find_slot(cache->slots, cache->capacity, term);
    if (cache->slots[slot].term != NULL) {
        cache->slots[slot].id = id;
        return 0;
    }
    char *owned_term = strdup(term);
    if (owned_term == NULL) {
        return -1;
    }
    cache->slots[slot].term = owned_term;
    cache->slots[slot].id = id;
    cache->count++;
    return 0;
}

/* Must be called with cache->mutex already held. Returns 1 and sets
 * *out_id if `term` is cached, 0 if not. */
static int term_cache_lookup_locked(TermCache *cache, const char *term, int64_t *out_id) {
    size_t slot = term_cache_find_slot(cache->slots, cache->capacity, term);
    if (cache->slots[slot].term != NULL) {
        *out_id = cache->slots[slot].id;
        return 1;
    }
    return 0;
}

/* One (term, id) pair resolved against Postgres but not yet known to be
 * durable -- see term_cache.h's TermCachePending doc comment for why
 * these can't go straight into the shared cache. */
typedef struct {
    char *term;
    int64_t id;
} PendingEntry;

struct TermCachePending {
    PendingEntry *entries;
    size_t count;
    size_t capacity;
};

TermCachePending *term_cache_pending_create(void) {
    TermCachePending *pending = malloc(sizeof(TermCachePending));
    if (pending == NULL) {
        return NULL;
    }
    pending->capacity = 16;
    pending->entries = malloc(sizeof(PendingEntry) * pending->capacity);
    if (pending->entries == NULL) {
        free(pending);
        return NULL;
    }
    pending->count = 0;
    return pending;
}

void term_cache_pending_free(TermCachePending *pending) {
    if (pending == NULL) {
        return;
    }
    for (size_t i = 0; i < pending->count; i++) {
        free(pending->entries[i].term);
    }
    free(pending->entries);
    free(pending);
}

/* Appends (term, id), taking a fresh strdup()'d copy of `term`. Returns
 * 0 on success, -1 on allocation failure. */
static int term_cache_pending_add(TermCachePending *pending, const char *term, int64_t id) {
    if (pending->count == pending->capacity) {
        size_t new_capacity = pending->capacity * 2;
        PendingEntry *new_entries = realloc(pending->entries, sizeof(PendingEntry) * new_capacity);
        if (new_entries == NULL) {
            return -1;
        }
        pending->entries = new_entries;
        pending->capacity = new_capacity;
    }
    char *owned_term = strdup(term);
    if (owned_term == NULL) {
        return -1;
    }
    pending->entries[pending->count].term = owned_term;
    pending->entries[pending->count].id = id;
    pending->count++;
    return 0;
}

/* Linear scan -- pending lists are scoped to one document's chunks at a
 * time (at most a few hundred distinct terms), same chunk-scale tradeoff
 * already made elsewhere in this codebase. Returns 1 and sets *out_id if
 * found. */
static int term_cache_pending_lookup(const TermCachePending *pending, const char *term,
                                      int64_t *out_id) {
    for (size_t i = 0; i < pending->count; i++) {
        if (strcmp(pending->entries[i].term, term) == 0) {
            *out_id = pending->entries[i].id;
            return 1;
        }
    }
    return 0;
}

int term_cache_commit_pending(TermCache *cache, TermCachePending *pending) {
    if (pending == NULL) {
        return 0;
    }
    int ok = 1;
    pthread_mutex_lock(&cache->mutex);
    for (size_t i = 0; i < pending->count && ok; i++) {
        if (term_cache_insert_locked(cache, pending->entries[i].term, pending->entries[i].id) != 0) {
            ok = 0;
        }
    }
    pthread_mutex_unlock(&cache->mutex);
    term_cache_pending_free(pending);
    return ok ? 0 : -1;
}

int term_cache_preload(TermCache *cache, PgStore *store) {
    PGresult *res = PQexec(store->conn, "SELECT id, term FROM terms;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "term_cache_preload: query failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    int rows = PQntuples(res);
    int ok = 1;
    pthread_mutex_lock(&cache->mutex);
    for (int i = 0; i < rows && ok; i++) {
        int64_t id = atoll(PQgetvalue(res, i, 0));
        const char *term = PQgetvalue(res, i, 1);
        if (term_cache_insert_locked(cache, term, id) != 0) {
            ok = 0;
        }
    }
    pthread_mutex_unlock(&cache->mutex);
    PQclear(res);
    return ok ? 0 : -1;
}

int64_t *term_cache_get_or_create_terms(TermCache *cache, TermCachePending *pending, PgStore *store,
                                         const char *const *terms, size_t count) {
    int64_t *ids = malloc(sizeof(int64_t) * count);
    if (ids == NULL) {
        return NULL;
    }

    /* Phase 1: check the shared cache (safe -- it only ever holds terms
     * already known to be durable), under the lock -- kept short (pure
     * in-memory work, no Postgres call while held) so other threads'
     * cache hits are never blocked behind this. */
    pthread_mutex_lock(&cache->mutex);
    for (size_t i = 0; i < count; i++) {
        int64_t id;
        ids[i] = term_cache_lookup_locked(cache, terms[i], &id) ? id : -1;
    }
    pthread_mutex_unlock(&cache->mutex);

    /* Then this document's own pending list -- terms an earlier chunk in
     * the SAME document already resolved but hasn't committed yet. */
    for (size_t i = 0; i < count; i++) {
        if (ids[i] != -1) {
            continue;
        }
        int64_t id;
        if (term_cache_pending_lookup(pending, terms[i], &id)) {
            ids[i] = id;
        }
    }

    /* Collect the distinct still-unresolved terms -- `terms` may itself
     * contain duplicates, and Postgres only needs to be asked once per
     * distinct term. Borrowed pointers into `terms`, freed as just the
     * array below, not its contents. */
    const char **unresolved = malloc(sizeof(char *) * count);
    if (unresolved == NULL) {
        free(ids);
        return NULL;
    }
    size_t unresolved_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (ids[i] != -1) {
            continue;
        }
        int already_queued = 0;
        for (size_t j = 0; j < unresolved_count; j++) {
            if (strcmp(unresolved[j], terms[i]) == 0) {
                already_queued = 1;
                break;
            }
        }
        if (!already_queued) {
            unresolved[unresolved_count++] = terms[i];
        }
    }

    if (unresolved_count == 0) {
        free(unresolved);
        return ids;
    }

    /* Phase 2: resolve whatever's still missing against Postgres, via
     * the calling thread's own connection -- outside the cache lock. */
    int64_t *resolved_ids = pg_store_get_or_create_terms(store, unresolved, unresolved_count);
    if (resolved_ids == NULL) {
        free(unresolved);
        free(ids);
        return NULL;
    }

    /* Phase 3: record newly resolved terms in the document-local pending
     * list, NOT the shared cache -- we don't yet know whether this
     * document's transaction will actually commit (see TermCachePending's
     * doc comment). term_cache_commit_pending() moves these into the
     * shared cache later, only once it's actually safe to. */
    int ok = 1;
    for (size_t u = 0; u < unresolved_count && ok; u++) {
        if (term_cache_pending_add(pending, unresolved[u], resolved_ids[u]) != 0) {
            ok = 0;
        }
    }

    if (!ok) {
        free(resolved_ids);
        free(unresolved);
        free(ids);
        return NULL;
    }

    /* Fill in this call's still-unresolved slots from resolved_ids --
     * O(count * unresolved_count) worst case, acceptable at chunk scale
     * (a few hundred terms at most), same tradeoff already made for
     * ingest_index_chunk_terms()'s own dedup loop. */
    for (size_t i = 0; i < count; i++) {
        if (ids[i] != -1) {
            continue;
        }
        for (size_t u = 0; u < unresolved_count; u++) {
            if (strcmp(unresolved[u], terms[i]) == 0) {
                ids[i] = resolved_ids[u];
                break;
            }
        }
    }

    free(resolved_ids);
    free(unresolved);
    return ids;
}
