/*
 * Bulk TSV ingestion (spec section 8 -- loading the full MS MARCO passage
 * collection for the MRR@10/Recall@K evaluation). A three-phase,
 * deferred-term-resolution pipeline -- see SPEED.md for the full design
 * history and why it replaced the original per-document, TermCache-based
 * approach (every deadlock ever measured under real write concurrency
 * traced back to Postgres's ON CONFLICT speculative insertion racing on
 * terms.term's unique index):
 *
 *   Phase 1 (this connection, single-threaded): COPY the whole TSV/CSV
 *   file into a staging table, documents_raw, in one pass -- no parsing,
 *   no per-row round trips.
 *
 *   Phase 2 (thread_count workers, each its own connection): claim
 *   independent row-number ranges out of documents_raw (plain SELECTs,
 *   nothing to lock), run the existing tokenize/stopword/lemmatize
 *   pipeline, insert real rows into passages, and stage each passage's
 *   term postings by their raw text into postings_staged -- never
 *   touching the terms table at all, which is what makes this genuinely
 *   contention-free rather than just less contended.
 *
 *   Phase 3 (this connection again, single-threaded): one set-based pass
 *   that resolves every distinct staged term into terms and writes the
 *   real postings rows, joining postings_staged against terms by text.
 *
 * pg_store.c's staging tables (documents_raw/postings_staged) are
 * created fresh and dropped again at the end of every call -- this
 * function owns their whole lifecycle.
 */

#ifndef LEXIS_BULK_INGEST_H
#define LEXIS_BULK_INGEST_H

#include <stddef.h>
#include <stdint.h>

#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"

/* Ingests every row of the file at `tsv_path` (one "<id><TAB><text>" row
 * per line, no header, CSV-quoted per RFC4180 -- see pg_store.h's
 * pg_store_copy_documents_raw() for why plain unquoted TSV isn't safe:
 * real MS MARCO passages contain literal, unescaped backslash and
 * double-quote characters) across `thread_count` Phase 2 worker threads,
 * each with its own connection (via `conninfo`) to the same database.
 * thread_count < 1 is treated as 1.
 *
 * If `schema_name` is non-NULL and non-empty, every connection this
 * function opens (the coordinator's and each Phase 2 worker's -- there is
 * no single shared connection here for a schema selection made elsewhere
 * to carry over from) calls pg_store_use_schema(schema_name) immediately
 * after connecting, so the whole run targets that schema. NULL/empty
 * skips this entirely, leaving every connection on whatever schema is
 * already the default (`public`) -- the pre-multi-corpus behavior,
 * unchanged, for any caller that doesn't care about corpora.
 *
 * Takes a schema_name directly rather than a corpus_id because
 * bulk_ingest_rebuild_corpus()'s temporary rebuild schema has no
 * public.corpora registry row of its own to look one up from -- callers
 * that DO have a corpus_id (i.e. everyone except that one) should look up
 * its schema_name via pg_store_create_corpus()/pg_store_list_corpora()
 * first. schema_name must be a trusted, server-generated identifier --
 * same constraint as pg_store_use_schema(), which this ultimately calls.
 *
 * Because Phase 1 loads the entire file via a single COPY, a single
 * malformed row (e.g. missing the tab/wrong column count) fails the
 * WHOLE load atomically -- there is no per-row skip-and-continue the way
 * the old streaming pipeline had, since COPY doesn't offer one. This is
 * a deliberate trade-off: a bulk loader's job is to trust the source
 * file's format, not silently drop rows from it (see the real corpus.tsv
 * format investigation in SPEED.md, which verified the actual MS MARCO
 * export is clean at full 8.84M-row scale before this pipeline was
 * built to depend on it). A Phase 2 batch (many documents sharing one
 * transaction, see bulk_ingest.c) that fails is retried a few times as a
 * fresh transaction before being logged and skipped -- costing at most
 * that batch's documents, not the whole run (see phase2_worker_run()'s
 * own comment for why this doesn't abort the rest of the pipeline).
 * Returns the total number of passages ingested (>= 0) on success, or -1
 * if the file can't be COPYed in, any worker's connection fails to open
 * (a full run needs every requested thread actually working), or
 * Phase 3's finalize fails. */
long bulk_ingest_tsv(const char *conninfo, const char *schema_name, const StopwordSet *stopwords,
                      const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                      const char *tsv_path, size_t chunk_size, size_t overlap,
                      int thread_count);

/* Rebuild-on-append: adds `new_document_names[i]`/`new_document_texts[i]`
 * (parallel arrays, length new_document_count) to `corpus_id`'s existing
 * documents and re-ingests the combined set from scratch, using the same
 * fast bulk pipeline above -- see APP_SPEC.md's "Adding documents to an
 * existing group" for the full design and why this replaces the whole
 * corpus rather than inserting into it live (in short: the deferred-
 * constraint/UNLOGGED machinery bulk_ingest_tsv() depends on for its
 * speed would otherwise have to run against a corpus's already-live
 * data, risking it for the duration of every append).
 *
 * A document whose name matches an existing one replaces it (the natural
 * reading of "re-add this file"). Every existing document not replaced
 * is carried forward unchanged, re-chunked from its original text (see
 * pg_store_insert_document()) exactly as if the whole combined set were
 * being ingested fresh -- chunk_size/overlap apply uniformly, so a
 * document's chunk boundaries stay consistent with the rest of the
 * corpus even after N rebuilds.
 *
 * Never mutates the corpus's existing schema in place: the combined
 * document set is ingested into a fresh scratch schema first, and only
 * swapped in for the corpus (pg_store_swap_corpus_schema()) once that
 * ingest fully succeeds. A failure at any point -- CSV materialization,
 * the ingest itself, the swap -- leaves the corpus's existing data
 * completely untouched and cleans up the scratch schema; the corpus is
 * never left half-rebuilt.
 *
 * Returns the total number of passages in the rebuilt corpus (>= 0) on
 * success, or -1 on failure (corpus_id doesn't exist, the combined CSV
 * couldn't be written, the ingest failed, or the swap failed). */
long bulk_ingest_rebuild_corpus(const char *conninfo, int64_t corpus_id, const char *const *new_document_names,
                                 const char *const *new_document_texts, size_t new_document_count,
                                 const StopwordSet *stopwords, const WordNetTable *wordnet,
                                 const Lemmatizer *lemmatizer, size_t chunk_size, size_t overlap, int thread_count);

#endif /* LEXIS_BULK_INGEST_H */
