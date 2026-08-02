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

## The real full corpus run, and the term cache fix

The full 8,841,823-passage MS MARCO corpus was actually bulk-ingested
(`lexis bulk-ingest`, 4 threads): **972.6 passages/sec average, 151.5
minutes total, 184 deadlocks (zero documents permanently dropped)**. This
undercut the synthetic-benchmark projection (~1866/sec, ~1.3 hours) by
roughly 2x. The 184 deadlocks are negligible against 8.84M documents --
that's not the gap. The more likely cause: the synthetic benchmark's
~20K-word vocabulary saturates almost immediately (most of its documents
hit `pg_store_get_or_create_terms()`'s cheap SELECT-only path), while
real language keeps introducing brand-new vocabulary deep into a corpus
this large (Zipf's law) -- the real ingest has 3,492,916 distinct terms,
so a much larger share of documents pay the network round trip for term
resolution throughout the *entire* run, not just at the start.

Fix: a shared, thread-safe in-memory term cache (`term_cache.c`) sitting
in front of `pg_store_get_or_create_terms()` -- every worker thread
shares one cache, so a term resolved by any thread (new or previously
known) costs zero further Postgres traffic for every other thread from
then on. **Real, measured result on the identical first 500,000 rows of
the corpus: 1654.6 passages/sec, vs. 972.6 passages/sec for the original
full run's overall average -- a genuine ~1.7x improvement**, not a
projection. Full-corpus re-ingest with the cache is expected to land
somewhere in the 90-105 minute range (vs. 151.5 minutes), though the very
large final terms table (3.49M rows) may erode the improvement somewhat
by the end of a full run versus this partial-corpus measurement.

**A real, serious bug was found and fixed during this work**: the first
version of the cache wrote a newly-resolved (term, id) pair straight into
the shared cache the moment `pg_store_get_or_create_terms()` returned an
id -- but that `INSERT` only really happens if the document's surrounding
transaction actually commits. If it later rolls back (e.g. a deadlock on
a *later* chunk in the same document), the term row never persists, but
the shared cache still claimed it did -- poisoning every future document
using that term for the rest of the run with a `term_id` that fails
`postings`' foreign-key constraint. Verified directly: running the fixed
benchmark against the same 500K-row slice produced zero foreign-key
violations (deadlocks still occur and retry normally, as expected). Fix:
newly resolved terms stay in a document-local `TermCachePending` list and
only get merged into the shared cache after that document's transaction
actually commits (`term_cache_commit_pending()`) -- discarded, not
committed, on rollback. Covered by a dedicated regression test
(`test_pending_discarded_on_rollback_does_not_poison_cache`).

## Re-swept thread count after the cache fix

The original thread-count sweep (above) was dominated by contention on
the `terms` unique index, which the term cache now mostly eliminates --
worth re-checking whether more threads helps once that's no longer the
limiting factor. Re-swept on a 200K-row slice (consistent across
configs, same machine, 8 physical/logical cores):

| Threads | Throughput |
|---|---|
| 4 | 1396.0/sec |
| **6** | **1542.4/sec (nominal peak)** |
| 8 | 1287.6/sec |
| 10 | 1125.4/sec |
| 12 | 1408.8/sec |
| 16 | 1341.5/sec |

427 deadlocks total across all 6 runs combined (~1.2M document-ingestions),
1 document permanently dropped after 3 retries -- both negligible, no sign
of the old contention collapse returning at higher thread counts.

Honest read: **the 1125-1542/sec spread here is roughly the same
magnitude as run-to-run noise** -- a separate 4-thread measurement on a
different 500K-row slice got 1654.6/sec, a ~16% swing from nothing but
re-running on different data, comparable to the differences between
threads in this table. 6 nominally won this single-run sweep and is now
`LEXIS_INGEST_THREADS` in main.c, but this isn't a confident ranking
(each config only ran once) -- more threads past 4-6 is not a real lever
on this machine. The actual win was the term cache itself (~1.7x); chasing
thread count further is diminishing returns on an 8-core machine where
Postgres itself needs some of that CPU too.

**Possible confound not controlled for**: this sweep ran as six back-to-
back configs with no cooldown between them on a fanless M2 MacBook Air.
Sustained multi-core load on a chassis with no active cooling is a real,
documented mechanism for clock throttling that a fan-equipped Mac
wouldn't show the same way -- the sweep's non-monotonic bounce (8/10 dip,
12/16 recover) is at least as consistent with drifting thermal state
across the ~15-minute session as with the thread count itself. No hard
telemetry either way (`pmset -g therm` doesn't report useful data on
Apple Silicon, and `powermetrics` needs an interactive sudo password this
environment doesn't have) -- flagging the confound rather than the
(likely partly thermal) ranking as the real finding.

## Native Postgres, synchronous_commit, and a failed batching attempt

Three further ideas explored after the thread re-sweep, to get well
under the "still feels slow" 90-105 minute projection:

1. **`synchronous_commit = off`** during ingestion (`pg_store_disable_
   synchronous_commit()`, called once per worker connection) -- every
   COMMIT returns once its WAL record reaches the OS, without waiting for
   a physical disk fsync. Safe here specifically because this is a
   rebuildable index build, not live/irreplaceable data: if ingestion
   crashes, the fix is re-running it, not recovering unflushed commits.
   **Measured: 1786.4/sec vs. 1542.4/sec (same 6-thread, 200K-row config)
   -- a real, free ~16%.**

2. **Native Postgres instead of Docker Desktop.** Docker Desktop on macOS
   runs everything inside a lightweight Linux VM, so even "localhost"
   traffic to the containerized Postgres crosses that VM boundary before
   reaching it. Installed postgresql@18 natively via Homebrew (already
   present as a dependency of the client library) on port 5434 --
   entirely separate from both the Docker instance (port 5433, used by
   the test suite at the time; later removed entirely once the test
   suite was verified passing against native Postgres too, see
   CURRENT_STATE.md) and this machine's pre-existing, unrelated
   postgresql@14 instance (port 5432, real data from other projects, never
   touched). `make pg-start`/`make pg-stop` manage it; it does not
   auto-start on login. `main.c`'s `LEXIS_DB_CONNINFO` now points here.

3. **Batching multiple documents per transaction -- tried, and reverted
   after it made things dramatically worse, not better.** The idea (see
   the "next optimizations" list below, written before this was actually
   tried): wrap N documents in one transaction with per-document
   SAVEPOINT isolation, amortizing the BEGIN/COMMIT round trip (and its
   fsync) across the whole batch. Implemented with a full mechanism
   (`ingest_document_from_text_in_batch()`, SAVEPOINT/ROLLBACK TO
   SAVEPOINT/RELEASE SAVEPOINT support in pg_store.c, whole-batch retry
   escalation, one-document-at-a-time fallback) and thoroughly tested at
   small scale -- but under real concurrent load it caused **severe,
   sustained contention, not occasional retries**: 109 passages ingested
   in 5+ minutes on one run, a process that had to be force-killed after
   timing out on another. Root cause: a batch transaction holds locks on
   every term it's touched *so far*, for its *entire duration* -- 20
   documents' worth of distinct terms, held for 20 documents' worth of
   time, instead of one document's worth of terms held for one
   document's worth of time. With 6 threads each running a long,
   wide-locking batch transaction simultaneously, the contention surface
   multiplied badly enough that even the whole-batch-retry escalation
   logic and the proven one-document-per-transaction fallback kept
   re-triggering rather than recovering. **Fully reverted** -- back to
   one document per transaction, which is structurally the right shape
   for this concurrency model (small, short-lived transactions, small
   lock footprint, so N threads rarely collide) even though it means
   more COMMITs. The SAVEPOINT primitives and
   `ingest_document_from_text_in_batch()` were removed entirely (not
   left as dead code) once nothing called them anymore -- this section is
   the permanent record of why, since the code itself is gone.

**Combined real result (native Postgres + synchronous_commit=off, no
batching), same 200K-row slice, 6 threads: 2211.3 passages/sec** -- up
from 972.6/sec (the original full-corpus run) and 1654.6/sec (term cache
alone, Docker). Projected full-corpus time: 8,841,823 / 2211.3 ≈ 66.6
minutes, down from the original 151.5 minutes (~2.3x). A smaller,
20K-row run of the same configuration measured only 579.1/sec --
consistent with cold-start contention (an empty term cache means every
thread races on the very same first few common words simultaneously)
dominating a short run's average in a way it doesn't over a longer one;
not a sign the 200K/2211.3 number is unrepresentative of a full run's
steady state. Both runs preserved data correctly (20,002/20,000 and
200,007/199,998 passages/distinct documents respectively -- 2 documents
dropped out of 200,000 after exhausting retries under contention, a
~0.001% loss rate consistent with the project's existing retry policy,
which deliberately doesn't retry forever).

## The three-phase deferred-term-resolution redesign

Every single deadlock measured in this whole document, without exception,
traced back to the same thing: `CONTEXT: while inserting index tuple ...
in relation "terms"` -- Postgres's `ON CONFLICT` speculative insertion on
`terms.term`'s unique index, colliding whenever two or more threads race
on the same new word. Nothing else (`passages`, `postings`) ever caused
contention -- `GENERATED ALWAYS AS IDENTITY` sequences are safe under
concurrency by construction. Real measurement at the previous best config
(native PG, `synchronous_commit=off`, term cache, 6 threads): 90
deadlocks across 200,009 documents, roughly 21x the deadlock *rate* of
the original Docker/4-thread run (184 across 8.84M docs).

Rather than continue tuning thread count/batch size around that
contention, the redesign below eliminates it structurally: worker threads
never touch the `terms` table at all until every other thread is done.

1. **Phase 1 -- raw append.** One `COPY` loads the whole TSV/CSV file into
   an `UNLOGGED` staging table, `documents_raw` (`row_num` identity PK,
   `pid`, `text`), via libpq's COPY protocol (`pg_store_copy_documents_
   raw()`, streaming the file client-side in 64KB chunks -- no per-row
   parsing on our side at all).

   This required re-investigating the real `corpus.tsv`'s format first:
   14,480 of 8,841,823 lines contain literal, unescaped backslash
   characters (LaTeX-style `\displaystyle`, `\%`, IPA pronunciation
   markers), and a smaller sample showed unescaped double quotes too --
   both unsafe for Postgres's default `COPY` TEXT format, which treats
   backslash as its escape character. Fix: re-exported the corpus from
   duckdb with explicit `FORMAT CSV` (RFC4180 quoting) instead of the
   original unquoted-TSV export, and verified byte-for-byte round-trip
   correctness directly against Postgres -- both via `psql \copy` on the
   real problem rows (pid 226, 2799, 4866, 5663, 8637) and via this
   project's own `pg_store_copy_documents_raw()` at 149,461-row scale --
   before trusting it at full corpus size.

2. **Phase 2 -- parallel, contention-free processing.** Worker threads
   claim independent `row_num` ranges out of `documents_raw` (plain
   `SELECT ... WHERE row_num >= $1 AND row_num < $2` -- nothing to lock,
   unlike `terms`' unique index), run the existing, unchanged tokenize ->
   stopword-filter -> lemmatize pipeline, insert real rows into
   `passages`, and stage each passage's term postings by their raw *text*
   (not a resolved `terms.id`) into another `UNLOGGED`, unconstrained
   staging table, `postings_staged`. `ingest_lemmatize_terms()` and a new
   `ingest_count_distinct_terms()` (the per-chunk dedup+frequency-count
   logic, extracted out of `ingest_index_chunk_terms()` so both the old
   per-document path and this one share it instead of drifting) were
   exposed from `ingest.c`/`ingest.h` for this. Because Phase 2 never
   touches `terms`, batching many documents into one transaction is
   *safe* here -- unlike the earlier, reverted batching attempt above,
   there's no shared unique index for a wide transaction to hold locks
   on. Each worker batches `BULK_PHASE2_BATCH_SIZE` (500) documents per
   transaction, with a few retries before giving up on just that batch
   (logged, not fatal to the run -- matches `concurrent_worker_run()`'s
   existing per-document-failure tolerance).

3. **Phase 3 -- finalize.** After every Phase 2 worker joins, one
   single-threaded, single-writer pass (`pg_store_finalize_terms_and_
   postings()`, zero contention risk by construction) resolves every
   distinct staged term into `terms` (`INSERT ... SELECT DISTINCT ...
   ON CONFLICT (term) DO NOTHING`), then writes the real `postings` rows
   by joining `postings_staged` against `terms` on text. `work_mem` is
   raised to 1GB for this connection only (a session-local `SET`, not a
   `postgresql.conf` change) since this is the one place in the whole
   pipeline actually running a large hash join/distinct. The `TermCache`
   module (`term_cache.c`/`.h`) was, at this point, still used by
   `concurrent_ingest.c`'s separate directory-ingestion path --
   `bulk_ingest.c` never depended on it, since Phase 2 has nothing for it
   to coordinate. Both `concurrent_ingest.c` and `term_cache.c` were
   later deleted outright once this pipeline proved faster and simpler
   to reason about than the directory-ingestion path -- see the "one
   ingestion pipeline" cleanup recorded in git history and
   `CURRENT_STATE.md`; this section is left as-is as the historical
   record of the state at the time this redesign shipped.

**Real measured result, 200K-row slice, native Postgres, 6 threads:
3490.9 passages/sec** (`lexis bulk-ingest`, wall clock 57.3s for 200,009
passages) -- up from the previous best of 2211.3/sec, a genuine further
~1.58x, and ~3.6x over the original 972.6/sec. Projected full-corpus
time: 8,841,823 / 3490.9 ≈ 42.2 minutes, down from the original 151.5.
Correctness verified directly, not just "it ran without crashing": 200,009
passages / 214,010 distinct terms / 5,052,759 postings with zero
duplicate `(term_id, passage_id)` pairs; staging tables confirmed dropped
after the run; pid 226 (one of the real backslash-containing rows from
the format investigation above) round-tripped with its literal
`\displaystyle` text intact; a real `lexis query "energy of a photon"`
against the resulting index returned pid 226 among genuinely relevant
top-5 BM25 results with correct scores.

### Per-phase profiling: Phase 3 is the real bottleneck, not Phase 2

Before this, it was only a guess which phase actually dominated wall-clock
time -- `bulk_ingest_tsv()` only reported total elapsed time. Added
per-phase `clock_gettime()` instrumentation (printed unconditionally on
success, same convention as `eval_run()`'s own progress printing) and
re-ran the identical 200K-row benchmark. Real breakdown:

| Phase | Time | Share of total |
|---|---|---|
| Phase 1 (raw append / COPY) | 576ms | 1.0% |
| Phase 2 (parallel processing, 6 threads) | 7,806ms | 13.6% |
| Phase 3 (finalize) | 49,172ms | **85.4%** |
| Total | 57,576ms | -- |

This inverts the intuitive assumption that the "embarrassingly parallel"
phase would dominate at scale. Phase 3 -- a single-threaded `INSERT ...
SELECT DISTINCT ... ON CONFLICT DO NOTHING` into `terms`, then an
`INSERT ... SELECT` joining `postings_staged` (5,052,759 rows at this
scale) against `terms` -- is over 6x slower than the six-thread parallel
phase that produced its input. Any further optimization effort belongs
here, not in Phase 2, which was already fast.

### Schema-only experiment: `UNLOGGED` + deferred `postings` PK

Zero C code changes -- tests whether Phase 3's cost is dominated by live
index maintenance and WAL generation, or by the join/resolve work itself.
Before the run: `ALTER TABLE postings DROP CONSTRAINT postings_pkey;`,
then `ALTER TABLE terms SET UNLOGGED; ALTER TABLE postings SET
UNLOGGED;` (`passages` could not be included -- see below). After the
run: `ALTER TABLE postings ADD PRIMARY KEY (term_id, passage_id);`, then
`SET LOGGED` on both. Real numbers, same 200K-row benchmark, both
reaching the identical durable/constrained end state:

| | Baseline | Experiment |
|---|---|---|
| Phase 1 | 451ms | 576ms |
| Phase 2 | 7,481ms | 7,806ms |
| Phase 3 | **30,373ms** | 49,172ms |
| `ADD PRIMARY KEY` (postings, 5,052,759 rows) | -- | 2,498ms |
| `SET LOGGED` (terms) | -- | 732ms |
| `SET LOGGED` (postings) | -- | 7,958ms |
| **Total** | **49,515ms** | **57,576ms** |

**Net: 57,576ms -> 49,515ms, a real but modest 14% reduction (~1.16x) --
not the ~38% Phase 3 alone looked like in isolation.** `SET LOGGED` on
`postings` alone cost 7,958ms, clawing back nearly half of Phase 3's raw
improvement -- confirming directly (not just suspected) that `SET
LOGGED` is not free: it's the same WAL-generation work as normal
inserts, paid in one lump sum at the end instead of spread across the
run, not eliminated. Correctness verified identical to every prior run
(200,009 / 214,010 / 5,052,759 passages/terms/postings, zero duplicate
postings even after the PK was added back retroactively).

`passages` could not be made `UNLOGGED` in this experiment -- Postgres
refuses to change a table's persistence while a still-`LOGGED` table
holds a foreign key referencing it, and `query_log.c`'s `search_results`
table (unrelated to bulk ingestion, only populated in testing mode)
references `passages`. Not a real blocker (`passages` isn't written by
Phase 3, the actual target, and Phase 2 was already fast), but worth
noting as a real dependency, not an oversight, if this is ever made
permanent.

**Verdict**: real, free win, worth keeping -- but on its own, nowhere
near enough to reach a 12-18 minute full-corpus target. The dominant
remaining cost is Phase 3's join/resolve work itself, which this
experiment deliberately did not touch (Phase 3's SQL and thread count
were left exactly as they were, per the point of isolating this one
variable). See the next section for what actually dominates that
remaining cost.

### Parallel query experiment: a red herring -- the real cost was foreign keys

Postgres's own docs are explicit that a plain `INSERT ... SELECT` never
gets a parallel plan at all -- the planner refuses parallelism for any
query that writes data, full stop, with the sole documented exception of
`CREATE TABLE AS SELECT` (CTAS), which *can* parallelize its `SELECT`
side. Verified directly via `EXPLAIN`: the plain `INSERT` (identical to
`pg_store_finalize_terms_and_postings()`'s real second statement) never
showed a `Gather` node, matching the docs exactly. A CTAS-equivalent bare
`SELECT` didn't either, at first -- Postgres's default cost model judged
a serial plan cheaper even after `ANALYZE`, requiring `parallel_setup_
cost = 0; parallel_tuple_cost = 0;` to force a parallel plan at this row
count (whether it would kick in naturally at full 8.84M-row scale,
without forcing costs, is untested).

Isolated all three variables against the same 5,052,759 staged rows
(terms pre-resolved, ~2.9s, held constant):

| Target | Constraints | Parallelism | Time |
|---|---|---|---|
| Real `postings` (current production path) | PK + both FKs live | off | 45,064ms |
| Real `postings`, PK + both FKs dropped | none | off | 6,931ms |
| New table via CTAS | none (new table) | off | 5,194ms |
| New table via CTAS | none (new table) | forced on | 4,962ms |

**Parallelism itself accounts for only ~230ms of the gap (5,194ms ->
4,962ms, ~4%) -- noise-adjacent, not a real lever at this scale.** The
actual finding: dropping `postings`' two foreign keys
(`postings_term_id_fkey`, `postings_passage_id_fkey`), which neither the
original suggestion nor this document's own earlier evaluation had
identified as a cost at all, accounts for the overwhelming majority of
the difference (45,064ms -> 6,931ms, a genuine ~6.5x). Combined with the
PK finding above, deferring FK constraints turns out to matter about as
much as deferring the PK, likely more -- a bigger, previously-invisible
lever than parallel query ever was. Every number here fully verified
correct afterward (postings' PK and both FKs re-added successfully
against the real, already-loaded data with zero violations; 200,009 /
214,010 / 5,052,759 counts unchanged). The temporary code used to pause
`bulk_ingest_tsv()` after Phase 2 for this test (an env-var-gated early
return) was fully reverted -- confirmed via an empty `git diff` on
`bulk_ingest.c` -- before anything else changed.

**Updated priority**: defer `postings`' foreign keys alongside its PK
(same zero-C-code, schema-only shape as the experiment above) before
spending any effort on parallel query or the hash-partitioned C refactor
-- neither is likely to matter much until the much larger FK-checking
cost is addressed first.

### Made permanent: `pg_store_prepare_bulk_load()` / `pg_store_finish_bulk_load()`

Implemented the combined finding as real, permanent code rather than a
one-off `psql` experiment: two new `pg_store.c` functions bracket Phase
3 in `bulk_ingest_tsv()`. `pg_store_prepare_bulk_load()` drops
`postings`' PK and both FKs (`IF EXISTS`, so a prior crashed run's
already-weakened state doesn't wedge the next one) and sets
`postings`/`terms` `UNLOGGED`; `pg_store_finish_bulk_load()` rebuilds the
PK and both FKs (while still `UNLOGGED`, so the build itself is free),
then restores `LOGGED` status. `passages` is deliberately left alone --
`query_log.c`'s `search_results` table holds a `LOGGED` FK referencing
it, and `passages` isn't written by Phase 3 anyway.

Hit and fixed one real ordering bug in the process, caught by a test, not
assumed correct: `SET LOGGED` has to happen on `terms` before `postings`
during restore (the reverse of the drop order) -- once `postings`' FK to
`terms` exists again, Postgres refuses to mark the referencing table
`LOGGED` while what it points at is still `UNLOGGED`. New tests
(`test_prepare_bulk_load_defers_constraints_and_durability`,
`test_finish_bulk_load_restores_constraints_and_durability_and_data_
survives`) caught this directly before it ever reached a real benchmark
run.

**Real measured result, full pipeline, 200K-row slice, native Postgres,
6 threads** (`bulk_ingest_tsv()` now reports five buckets, not three):

| Phase | Time |
|---|---|
| Phase 1 (raw append) | 459ms |
| Phase 2 (parallel processing) | 8,277ms |
| Prepare (defer constraints) | 15ms |
| Phase 3 (finalize) | 5,060ms |
| Restore (rebuild constraints) | 11,892ms |
| **Total** | **25,712ms** |

**7,778.8 passages/sec -- a 2.24x speedup over the original 57,576ms
baseline, and 1.93x over the Step 1-only (PK+UNLOGGED, no FK deferral)
result of 49,515ms.** Projected full-corpus time: 8,841,823 / 7,778.8 ≈
**18.9 minutes**, down from the original 151.5 minutes and from this
redesign's own earlier 42.2-minute projection -- landing at the edge of
the 12-18 minute range the original external suggestion predicted,
despite that suggestion never having identified foreign keys as the
real lever. Correctness verified identical to every prior run (200,009 /
214,010 / 5,052,759 passages/terms/postings, zero duplicate postings,
schema fully restored -- PK, both FKs, both tables LOGGED -- staging
tables dropped, pid 226's backslash content intact, a real `lexis query`
returning the same relevant top-5 results). Not yet run at full 8.84M-row
scale.

## Query-side slowness at full corpus scale: shared_buffers + CLUSTER

Discovered while trying to run the full 6,980-query MS MARCO dev eval:
even with zero LLM involvement (`--no-llm-expansion`, see eval.h), a
50-query sample averaged ~17.9 sec/query -- a projected ~34.7 hours for
the full set. This is `bm25.c`'s `bm25_accumulate_term_scores()` query
(`SELECT passage_id, term_frequency, token_count FROM postings WHERE
term_id = $1;`) being genuinely slow at 226M-row scale, not an LLM
problem at all.

Root-caused directly via `EXPLAIN (ANALYZE, BUFFERS)` against a real
common term (`"use"`, term_id 1679349, document frequency 1,335,518 --
appears in ~15% of the entire corpus):

```
Before: Bitmap Heap Scan, Heap Blocks: exact=800505, Buffers: shared hit=5124 read=800505
        Execution Time: 13790.541 ms
```

Two compounding causes: `shared_buffers` was still Postgres's untouched
128MB default (this project had only ever tuned `work_mem`, a
per-connection setting -- `shared_buffers` requires a config file edit
and a full restart, and had simply never come up before this
investigation), against an 11GB `postings` table -- almost nothing could
stay cached. Worse: 1,335,518 matching rows spread across 800,505 heap
blocks is only ~1.67 rows/block, confirming physical row order (just
insertion order from Phase 2/3) has zero correlation with term_id --
fetching one term's postings meant ~800K effectively *random* page
reads, the worst-case access pattern for any storage medium.

**Fix, two parts:**
1. `shared_buffers` raised from 128MB to 1GB in `postgresql.conf` (native
   instance, `/opt/homebrew/var/postgresql@18/postgresql.conf`), applied
   via `make pg-stop`/`make pg-start` (this setting requires a restart,
   not just a reload). Sized against this machine's real 8GB ceiling and
   the local LLM's ~5.7GB worst-case footprint when both are loaded
   together, not the textbook "25% of RAM" a dedicated DB server would
   use.
2. `CLUSTER postings USING postings_pkey;` -- physically reorders the
   whole table to match `(term_id, passage_id)` order, so one term's
   rows become contiguous instead of scattered. A real, measured
   operation on the actual 226,770,750-row table, not a projection: **6
   minutes 47 seconds total**, verified live via Postgres's own
   `pg_stat_progress_cluster` view (the expensive full-table scan/rewrite
   -- all 226,770,750 tuples, all 1,444,407 blocks -- finished by minute
   6:45; the final index-rebuild phase took only ~2 more seconds).
   Followed by `ANALYZE postings;` (2.6s).

**Real result, identical query, same term, after both fixes:**

```
After:  Bitmap Heap Scan, Heap Blocks: exact=8507, Buffers: shared hit=3 read=13628
        Execution Time: 174.506 ms
```

**13,790.5ms -> 174.5ms -- a real, measured ~79x speedup**, ahead of the
10-50x projected beforehand. Heap blocks needed dropped from 800,505 to
8,507 (~157 rows/block now, vs. 1.67 before -- confirms the clustering
worked as intended, not just correlated improvement). Correctness
verified unchanged: identical passage/term/posting counts
(8,842,136/3,492,916/226,770,750), `postings_pkey` still present with a
`CLUSTER` marker confirming Postgres registered it, both FKs intact, and
a real `lexis query` still returning correct results -- that same
interactive query dropped from needing ~109s in an earlier full-pipeline
test to **9.2s total** (model load + search + generation).

Not yet made permanent/automatic: `CLUSTER` doesn't stay in effect as
new rows are inserted (Postgres doesn't maintain physical order on
future writes), so a future bulk-ingest run would need to re-cluster --
this was a manual, one-time fix against the current data, not yet wired
into `bulk_ingest_tsv()`'s pipeline. `shared_buffers = 1GB` is a
permanent `postgresql.conf` change and persists across restarts as-is.

### Full 6,980-query eval, naked BM25 (`--no-llm-expansion`), post-fix

Real, complete run against the full MS MARCO dev query set, zero LLM
calls, zero WordNet expansion -- plain lemmatized query terms straight
into `bm25_search()`:

```
Queries evaluated: 6980 (skipped 0)
MRR@10:      0.1869
Recall@10:   0.3877
Recall@100:  0.6618
Total time:  25.0 minutes
```

25.0 minutes for all 6,980 queries, down from a ~34.7-hour projection
before the `shared_buffers`/`CLUSTER` fixes -- consistent with the
~79x single-query improvement measured directly above. MRR@10=0.1869
lands in the range commonly cited for default-parameter BM25 baselines
on MS MARCO passage dev (e.g. Anserini's default BM25, ~0.18-0.19) --
this is a real, complete, directly comparable number, not a sample
extrapolation. LLM-expansion path not yet re-run at full scale post-fix;
see "Next optimizations" for the comparison this sets up.

## Next optimizations, roughly in priority order

1. **Shared in-memory term cache across worker threads, refined from the
   original "pre-load the vocabulary" idea below after the real
   8.84M-passage MS MARCO bulk-ingest measurably underperformed the
   synthetic-corpus benchmark (~1,126-1,184 passages/sec actual vs. the
   ~1,866/sec peak measured on the earlier 3,000-doc benchmark corpus) --
   real evidence the much larger real vocabulary drives meaningfully more
   deadlock/retry overhead than the benchmark predicted. Design: one
   mutex-guarded hash map (`term -> id`, modeled on `wordnet.c`'s
   bucket-chained table) shared by every worker thread instead of each
   thread's private, isolated `PgStore` connection today. (a) Pre-load it
   with one `SELECT id, term FROM terms` before spawning workers -- any
   previously-known term costs zero Postgres traffic ever again. (b) The
   moment any thread successfully creates a genuinely new term, it writes
   `term -> id` into the shared map before continuing, so a *different*
   thread needing that same term moments later finds it in-process (a
   mutex lock + hash lookup, single-digit microseconds) instead of racing
   Postgres (a ~0.13ms round trip in the best case, a full transaction
   abort + whole-document reingest in the deadlock case). Given MS
   MARCO's word frequencies are Zipfian, this should eliminate the large
   majority of contention, since a small number of common words getting
   hit by multiple threads repeatedly is the likely dominant cause. Does
   *not* fully close the race for two threads discovering the exact same
   brand-new word in the same instant, before either's Postgres insert
   completes -- fully closing that needs a "single-flight" pattern
   (writing a *pending* placeholder into the map under the lock so a
   second thread waits on a condition variable instead of also hitting
   Postgres), meaningfully more concurrency code than the simple
   cache -- worth building only if the simple cache alone doesn't get
   contention low enough once measured. Original, simpler version of this
   idea (a one-time whole-corpus pre-scan pass before any concurrent work
   starts, rather than a live/shared cache) kept below for context: it
   would still work, but duplicates the tokenize/lemmatize work in a
   separate pass and doesn't help with terms only discovered mid-run,
   which the live-shared-cache design handles for free.

2. ~~Batch multiple documents per transaction, not just per document.~~
   **Tried against the old per-document pipeline and reverted -- see
   "Native Postgres, synchronous_commit, and a failed batching attempt"
   above -- then successfully reintroduced under the three-phase redesign
   (see "The three-phase deferred-term-resolution redesign" above).** The
   original attempt's round-trip-amortization logic was sound, but it
   ignored the lock-footprint cost: a batch transaction holds every term
   it's touched for its whole duration, which under real 4-6 thread
   concurrency caused severe, sustained contention (109 passages ingested
   in 5+ minutes on one run). The fundamentally different approach that
   entry said this would need turned out to be Phase 2's design itself:
   batching is only dangerous when the batched transaction touches a
   shared unique index (`terms`), so a pipeline that structurally never
   does that during the batched phase can batch freely. 500 documents per
   transaction, zero contention, real measured throughput above.

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

5. **Lower priority:** Postgres server tuning (native postgresql@18's
   default `postgresql.conf` settings are conservative, untouched so far
   beyond `work_mem` -- `shared_buffers`, `max_connections`), and
   resumability (nothing currently lets a crashed run pick up where it
   left off).
