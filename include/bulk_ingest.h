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
long bulk_ingest_tsv(const char *conninfo, const StopwordSet *stopwords,
                      const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                      const char *tsv_path, size_t chunk_size, size_t overlap,
                      int thread_count);

#endif /* LEXIS_BULK_INGEST_H */
