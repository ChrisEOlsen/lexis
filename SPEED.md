# Ingestion Speed — Status and Next Optimizations

Tracks what's been measured and what's left to try for ingestion
throughput, specifically against the spec's MS MARCO benchmark target
(spec 8.2, 8,841,823 passages). See LIMITATIONS.md for the file-by-file
tradeoff log this complements.

## Where things stand

| Approach | Best throughput | MS MARCO (8.84M passages) projection |
|---|---|---|
| SQLite, single-threaded (transaction-batched) | 552.9-569.9 passages/sec | ~4.8 hours |
| SQLite, sharded (experiment/sharded-ingestion, 4 threads) | 735.3 passages/sec | ~3.3 hours |
| Postgres, concurrent only (no batching) | ~165.9 passages/sec | worse than SQLite single-threaded |
| Postgres, batched, single-threaded | 476.7 passages/sec | ~5.2 hours |
| Postgres, batched + concurrent (synthetic corpus, 4 threads) | 1866.1 passages/sec | ~1.3 hours |
| **Postgres, batched + concurrent (real MS MARCO chunk, 4 threads)** | **1920.7 passages/sec** | **~1.28 hours** |

The first five rows are measured on a synthetic 3,000-document benchmark
corpus built to match MS MARCO's published characteristics (~56
words/passage, comparable vocabulary diversity). The last row is a real
measurement, not an extrapolation: 50,004 passages from an actual chunk of
MS MARCO's passage corpus (pulled via a Hugging Face parquet mirror,
BeIR/msmarco, after Microsoft's own blob storage started returning
`PublicAccessNotPermitted` for the URL the spec's own README references),
ingested end-to-end through the real `lexis ingest` CLI (not a standalone
benchmark harness) at 4 threads: 26,034ms, 82,001 distinct terms, 1,253,901
postings. Confirms the synthetic-corpus numbers above were a good proxy --
real MS MARCO came in essentially the same, slightly better. A live query
against this real chunk ("What was the purpose of the Manhattan
Project?") retrieved the right passages and generated a correct, grounded
answer, confirming the full pipeline, not just ingestion, works against
real data.

Thread-count sweep on this machine (8 logical cores), post-batching:

| Threads | Throughput |
|---|---|
| 1 | 476.7/sec |
| 2 | 1055.4/sec |
| 3 | 1297.3/sec |
| **4** | **1866.1/sec (peak)** |
| 5 | 1658.6/sec |
| 6 | 1768.1/sec |
| 8 | 1195.2/sec |
| 12 | 1085.3/sec |
| 16 | 1184.9/sec |
| 24 | 293.2/sec (worse than 2 threads) |
| 32 | 151.0/sec (worse than 1 thread) |

Root cause of the collapse past ~16 threads: contention on the `terms`
unique index. Postgres's `ON CONFLICT` uses speculative insertion
internally, and concurrent speculative inserts on the same index can
deadlock regardless of `DO NOTHING` vs `DO UPDATE` (verified directly).
`concurrent_ingest.c` retries a whole document up to 3 times on failure to
recover -- correctness held at every thread count tested (zero documents
dropped), but retries cost real throughput at high concurrency.

## Next optimizations, roughly in priority order

1. **Pre-load the entire term vocabulary before the concurrent phase
   starts.** MS MARCO is static, known text -- the vocabulary doesn't need
   to be discovered incrementally. One pass that tokenizes/lemmatizes the
   whole corpus, collects every distinct term, and bulk-inserts them all
   into `terms` before any concurrent worker touches a passage would mean
   `pg_store_get_or_create_terms()` almost never hits its INSERT path
   during the actual parallel run -- just fast SELECT-only lookups. This
   attacks the deadlock/contention problem at its root rather than
   retrying around it, and likely unlocks higher thread counts too, since
   the collapse above was contention-driven, not raw CPU-scheduling
   overhead (see the discussion in chat history for the reasoning that
   separates these two).

2. **Batch multiple documents per transaction, not just per document.**
   Each document currently costs ~4-5 round trips (begin, batched term
   resolve, batched posting insert, commit). At 8.8M documents that's
   still tens of millions of round trips even in the best case. Grouping
   e.g. 50-100 documents into one transaction with bulk multi-row inserts
   would cut this further. Real tradeoff: coarser failure granularity --
   one bad document in a batch would need per-document handling inside
   the batch to avoid rolling back everything else in it, more bookkeeping
   than the current clean one-document-one-transaction model.

3. **Sharper retry logic.** `concurrent_ingest.c` currently retries *any*
   `ingest_document()` failure up to 3 times, not specifically deadlocks.
   A genuinely broken document (unreadable file, real error) gets retried
   uselessly before giving up. Checking Postgres's actual SQLSTATE
   (`40P01` = deadlock_detected) and only retrying on that would make
   failures surface faster and keep the logs trustworthy.

4. **A post-ingestion verification pass.** Confirm the final passage count
   matches the corpus size exactly after a real run, rather than trusting
   scattered log output over an hour-plus job. Cheap, and it's the actual
   proof of "clean," not an assumption.

5. **Lower priority:** Postgres server tuning (default Docker settings are
   conservative, untouched so far -- `shared_buffers`, `work_mem`,
   `max_connections`), and resumability (nothing currently lets a crashed
   run pick up where it left off).
