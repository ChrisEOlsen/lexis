/*
 * Shared, thread-safe in-memory term cache -- an ingestion-time
 * optimization, not a persistence concern (see pg_store.c for that).
 * Sits in front of pg_store_get_or_create_terms(), turning "already
 * resolved by ANY worker thread, even moments ago" into a fast in-process
 * hash lookup instead of a Postgres round trip.
 *
 * Real motivation: measured directly against the full 8.84M-passage MS
 * MARCO ingest, real throughput (~972-1184 passages/sec) undercut the
 * synthetic-benchmark projection (~1866 passages/sec) by roughly 2x, and
 * it wasn't deadlock/retry contention -- only 184 deadlocks occurred
 * across the entire run, negligible next to 8.84M documents. The more
 * likely cause: the small synthetic benchmark corpus's ~20K-word
 * vocabulary saturates almost immediately, so most of its documents hit
 * pg_store_get_or_create_terms()'s cheap SELECT-only path. Real language
 * keeps introducing brand-new vocabulary deep into a corpus this large
 * (Zipf's law) -- the real ingest has 3.49 million distinct terms -- so a
 * much larger share of documents pay the network round trip for term
 * resolution throughout the *entire* run, not just at the start. This
 * cache attacks that directly: every worker thread shares one cache, and
 * the moment any thread resolves a term (new or previously known), every
 * other thread finds it in-process from then on, no Postgres round trip
 * at all. See SPEED.md/LIMITATIONS.md.
 */

#ifndef LEXIS_TERM_CACHE_H
#define LEXIS_TERM_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "pg_store.h"

typedef struct TermCache TermCache;

/* Allocates an empty, thread-safe term cache. Returns NULL on allocation
 * failure. */
TermCache *term_cache_create(void);

/* Frees the cache and everything in it. Safe to call with cache == NULL. */
void term_cache_free(TermCache *cache);

/* Populates the cache with every term currently in `store`'s terms
 * table -- a one-time bulk SELECT before any concurrent worker starts,
 * so vocabulary already known from a prior ingestion run costs zero
 * Postgres traffic during this one. Not required before using the cache
 * (term_cache_get_or_create_terms() resolves cache misses against
 * Postgres itself, same as always) -- purely a head start. Returns 0 on
 * success, -1 on a database or allocation failure. */
int term_cache_preload(TermCache *cache, PgStore *store);

/* Accumulates (term, id) pairs newly resolved against Postgres during one
 * document's transaction, but NOT YET known to be durable -- a document's
 * writes (see ingest_document_from_text()) all live inside one
 * transaction, and Postgres's INSERT ... ON CONFLICT ... RETURNING id
 * only really happened if that transaction actually commits. Writing a
 * newly "created" term straight into the shared TermCache the moment
 * pg_store_get_or_create_terms() returns an id -- this module's first,
 * broken design -- was verified directly to cause real, cascading
 * failures: if that same transaction later rolls back for any reason
 * (e.g. a deadlock on a *later* chunk in the same document), the term row
 * never actually persists, yet the shared cache still claims it exists --
 * poisoning every future document that uses that term for the rest of
 * the run with a term_id that fails postings' foreign key constraint.
 * The fix: keep newly resolved terms document-local until the document's
 * transaction actually commits (term_cache_commit_pending()), discarding
 * them instead (term_cache_pending_free()) if it rolls back. See
 * LIMITATIONS.md. */
typedef struct TermCachePending TermCachePending;

/* Allocates an empty pending list. Returns NULL on allocation failure. */
TermCachePending *term_cache_pending_create(void);

/* Discards a pending list WITHOUT touching the shared cache -- call this
 * when the document's transaction rolled back, since anything in it was
 * never actually made durable. Safe to call with pending == NULL. */
void term_cache_pending_free(TermCachePending *pending);

/* Merges every (term, id) pair in `pending` into `cache` (under the
 * lock) and frees `pending` -- call this only AFTER the document's
 * transaction has actually committed, since that's the point these
 * terms became durable. Returns 0 on success, -1 on allocation failure
 * (pending is still freed either way). */
int term_cache_commit_pending(TermCache *cache, TermCachePending *pending);

/* Same contract as pg_store_get_or_create_terms() -- resolves every term
 * in `terms[0..count)` (which may contain duplicates) to its id,
 * returning a newly allocated array of `count` ids in the same order
 * (caller must free()). Checks the shared cache first, then `pending`
 * (terms this same document already resolved in an earlier chunk),
 * before making a Postgres round trip at all (via the existing batched
 * pg_store_get_or_create_terms(), using `store` -- the calling thread's
 * own connection) for whatever's still genuinely unknown. Newly
 * resolved terms are added to `pending`, NOT the shared cache directly
 * -- see TermCachePending for why. Requires count >= 1. Returns NULL on
 * failure. */
int64_t *term_cache_get_or_create_terms(TermCache *cache, TermCachePending *pending, PgStore *store,
                                         const char *const *terms, size_t count);

#endif /* LEXIS_TERM_CACHE_H */
