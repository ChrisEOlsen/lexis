# APP_SPEC.md

Design for turning LEXIS from a single-corpus CLI tool into a Qt/C++
desktop application supporting multiple independent document collections
("groups") and multi-format ingestion (PDF, DOCX, images/scans, CSV, TXT).
This is a target-state design document, not a record of what's built --
see `CURRENT_STATE.md` for what actually exists today (single-corpus,
TSV-only, CLI-only). Nothing described here has been implemented yet.

Context/license: LEXIS is a non-commercial, open-source project aimed at
privacy-conscious users (local-only search and generation, no external API
calls at query time). Dependency choices below are made on build-footprint
and contributor-friction grounds, not license-avoidance -- GPL dependencies
are not disqualifying here, but are still weighed against simpler,
smaller-footprint alternatives when one exists.

## Core concept: groups = one Postgres schema each

A "group" is the user-facing unit of organization -- a named collection of
documents the user can search independently of every other group (e.g.
"Tax Records 2024", "Research Papers"). Under the hood, one group is one
Postgres schema, holding its own `passages`/`terms`/`postings` tables --
the exact schema `CURRENT_STATE.md` documents today, just no longer
assumed to be the only one in the database.

**Registry.** A `public.corpora` table tracks what groups exist:

```sql
CREATE TABLE corpora (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    display_name TEXT NOT NULL,
    schema_name TEXT NOT NULL UNIQUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

`schema_name` is server-generated (`corpus_1`, `corpus_2`, ...), never
built from user input. Postgres identifiers can't be parameterized the way
`$1` values can, so user-supplied group names never become part of a raw
`CREATE SCHEMA`/`ALTER TABLE` statement -- `display_name` is a value in
this table, `schema_name` is an opaque, internally-assigned identifier.
This closes the identifier-injection question entirely rather than trying
to sanitize arbitrary user text into a safe identifier.

**Connection scoping.** Opening a group sets `search_path` once per
connection:

```sql
SET search_path TO corpus_7, public;
```

Every existing query in `pg_store.c`/`bm25.c`/`bulk_ingest.c` references
`passages`/`terms`/`postings` unqualified today and keeps doing so
unchanged -- `search_path` resolves them to whichever group is active.
The only new code this requires is in the connection-open path
(`pg_store_open()` gains a corpus/schema parameter), not in the ~20
existing query call sites.

**Why schema-per-corpus, not a shared `corpus_id` column.** Considered and
rejected: one set of tables with a `corpus_id` column on every row (the
standard multi-tenant SaaS pattern). Rejected because:

- Every bulk-ingest optimization already built and measured
  (`pg_store_prepare_bulk_load()`/`finish_bulk_load()`'s `UNLOGGED` +
  dropped PK/FK, `CLUSTER`) operates on the whole table. In a shared-table
  design those operations touch every group's data at once, forcing
  ingestion into one group to either lock out or endanger every other
  group's live queries. Schema isolation means ingesting into group B
  never touches group A's tables at all -- the existing pipeline is
  reusable exactly as-is, per group.
- `DROP SCHEMA corpus_N CASCADE` is a metadata-only, near-instant, atomic
  group deletion. The shared-table equivalent (`DELETE ... WHERE
  corpus_id = N`) is a large, slow, lock-sensitive operation over
  however many rows every group combined has accumulated.
- BM25 correctness: document frequency and average passage length need
  to be scoped to one group's corpus, not blended across all groups.
  Schema isolation gives this for free (the same unqualified queries
  `bm25.c` already runs); a shared table needs `WHERE corpus_id = $1`
  added to every relevant query.

Trade-off accepted: schema DDL (`LEXIS_SCHEMA_SQL`) runs once per group
instead of once globally, so a future column addition means iterating
over every group's schema rather than one `ALTER TABLE`. Acceptable given
the number of groups a desktop app's user will realistically create.

## Adding documents to an existing group

**Decision: rebuild-on-append.** Adding files to a group that already has
data means: pull the group's existing passages back out, union with the
new documents, and run the existing three-phase fast bulk pipeline
(`bulk_ingest.c`) into a fresh copy, then swap it in for the group's
schema. This is a deliberate simplification -- see "Rejected approaches"
below for what this avoids.

Why this is safe and matches existing design: the pipeline never mutates
the live group's data mid-operation (it builds a fresh replacement), so
the existing "rebuildable, not crash-safe mid-run" trade-off documented in
`CURRENT_STATE.md` for a from-scratch ingest applies unchanged -- a crash
during an append rebuild is recoverable the same way a crash during
initial ingest already is, and never corrupts or loses the group's
prior, already-committed data (that data isn't touched until the rebuilt
copy successfully replaces it).

**Deferred, not built now:** a fast-path for small appends (below some
document-count threshold) that inserts directly against the live,
constrained table instead of rebuilding -- avoids the full-rebuild cost
for "drag in one more file" but is meaningfully slower per document than
the bulk path (live PK/FK index maintenance is incremental, not built
once in a sorted bulk pass, and gets slower as the group's existing index
grows). Worth adding once real usage shows rebuild cost matters for
common small-append cases; not required for a working v1.

**Rejected approaches:**

- *True incremental live inserts as the general case.* Scales with the
  group's existing size, not the append's size -- appending a large batch
  to an already-large group would be slower than a fresh rebuild of the
  same total size, not faster. Unacceptable as the default strategy.
- *Lucene-style segments with background merge.* The technically-correct
  scalable answer (what Anserini/Lucene actually do to solve this same
  problem), but requires combining BM25 stats (document frequency,
  average passage length) across segments at query time -- real,
  permanent added complexity. Disproportionate to this app's actual scale
  (personal/corporate document collections, thousands of documents, not
  MS MARCO's millions) and explicitly ruled out by the requirement to not
  compromise scoring quality. Documented here as the future option if a
  group's scale ever outgrows rebuild-on-append.

## Document ingestion: format support

**Common contract.** Every format's extraction step has exactly one job:
produce a plain `char *` of text. That string feeds `ingest.c`'s existing
`ingest_split_words()` -> `ingest_chunk_words()` -> `tokenize()` ->
`stopwords_filter()` -> `ingest_lemmatize_terms()` pipeline completely
unchanged. All new format-support work is a thin adapter layer upstream of
`ingest.c`; nothing downstream of "get me this file's text" changes.

| Format | Approach |
|---|---|
| TXT | `ingest_read_file()` -- already exists, no new work. |
| CSV | New in-house, strict RFC 4180 parser. |
| DOCX | New in-house extractor (ZIP + XML). |
| PDF (text layer) | Qt's `QtPdf` module. |
| PDF (scanned) / images | Tesseract OCR. |

**CSV.** Real corporate CSV exports routinely contain quoted fields with
embedded commas, newlines, or escaped quotes -- the naive delimiter-split
`bulk_ingest.c`'s Phase 1 uses today is correct only because MS MARCO's
TSV export is clean and uniform by construction. A dropped-in CSV needs a
real RFC 4180-compliant parser; a naive split would silently misparse
those fields into garbage instead of failing, which is the opposite of
this project's requirement. Malformed input (wrong field count for the
file's header, an unterminated quote) fails the whole file, matching
Phase 1's existing "one malformed row fails the entire load" philosophy
for bulk input -- no silent partial ingest. Default v1 document mapping:
one row = one document, every column's value concatenated into the
indexed text. Selecting a specific text-bearing column and treating the
rest as structured metadata is a real future enhancement, deferred --
the current `passages` schema has no place to put structured per-column
metadata even if it were extracted.

**DOCX.** A DOCX file is a ZIP archive containing `word/document.xml`,
whose `<w:t>` elements hold the actual text runs. New in-house extractor:
a permissively-licensed zip-read library plus a permissively-licensed XML
parser, pulling text runs out of that one file. Considered and rejected:
DocWire SDK (the current name for what was pitched as "DocToText") --
technically capable (nearly 100 formats, active project), but GPL-2.0-only
or paid-commercial licensed, and confirmed to require a heavy build
(vcpkg, C++20, an autotools bootstrap chain for its own third-party
dependencies, wrapping other libraries like `wv2` internally for legacy
Word parsing). Disproportionate build weight and contributor friction for
a narrow "DOCX to plain text" need, even with GPL itself no longer a
blocker for this project.

**PDF, text layer.** Qt's own `QtPdf` module: `QPdfDocument::getAllText(page)`
looped over every page, concatenated. Confirmed real, current API (Qt
6.8 docs). Chosen over `poppler-cpp` (more mature, purpose-built for
extraction, but a separate dependency with its own build chain --
freetype, fontconfig, etc.) specifically because `QtPdf` ships as part of
Qt: no additional dependency for contributors to build or for the app to
bundle, beyond what the UI already requires. License note: `QtPdf` itself
is LGPLv3/GPLv2/Qt-Commercial (PDFium underneath, bundled inside it, is
BSD) -- this is the same licensing question already live for using Qt at
all in this app, not a new, separate encumbrance the way a GPL PDF library
would have been on top of a permissively-licensed UI toolkit. Open item:
confirm which Qt license track (open-source LGPL/GPL vs. paid commercial)
the app is building against -- LGPL specifically expects dynamic linking
so users can relink a different Qt build, a real constraint on how the
app gets packaged.

**Images and scanned PDFs.** Tesseract OCR (Apache 2.0, proper C++ API,
`tesseract::TessBaseAPI`). Scanned-PDF detection: run the text-layer
extraction first; if the extracted text length is implausibly short
relative to the PDF's page count, treat it as scanned rather than
text-bearing, render its pages to images (`QtPdf` can do this too), and
OCR those images instead. Flagged explicitly, not treated as solvable by
better engineering: OCR is inherently slower per page than any text-layer
extraction path, by a wide margin. No library choice removes that cost --
only parallelism (see below) and Tesseract's own engine-mode speed/
accuracy trade-off are real levers. A corpus with a meaningful fraction of
scanned documents will take noticeably longer to ingest than an
all-text-layer corpus of the same size, and that's expected, not a bug.

## Performance: extraction as a parallel stage

Target scale is thousands of documents (personal/corporate collections),
not MS MARCO's millions -- this shapes the rebuild-on-append and
non-segmented decisions above, and shapes this too.

Format extraction (PDF/DOCX/CSV parsing, OCR) is CPU-bound, per-file, and
independent of every other file -- the same shape `bulk_ingest.c`'s
existing Phase 2 worker pool already exploits for chunking/tokenizing
`documents_raw` rows. Extraction should be implemented as a new step at
the front of that same worker-pool model (each worker: detect the file's
format, extract to plain text, then run the existing chunk/tokenize/stage
logic on the result) rather than as a separate pipeline stage with its
own synchronization -- reuses the concurrency model already built and
measured instead of inventing a second one.

Everything downstream of "plain text in hand" -- chunking, term staging,
the deferred-constraint bulk load, `CLUSTER` -- is the existing pipeline,
unchanged, and inherits all throughput work already measured in
`SPEED.md`. Rebuild-on-append uses this same pipeline for both a group's
first ingest and every subsequent append; there is no second, slower
ingestion code path to maintain.

## Explicitly out of scope for this spec

- Per-column CSV field selection / structured metadata UI.
- Incremental (non-rebuild) small-batch append fast path.
- Segment-based indexing (Lucene-style, with query-time stat merging).
- Cross-group / multi-group simultaneous search.

## Open items

- Specific permissive zip-read and XML parser libraries for the in-house
  DOCX extractor -- the approach is settled (in-house over DocWire), the
  exact libraries to build it from are not.
- UI/UX specifics beyond "drag files into a group, switch active group" --
  left to implementation judgment; the bar is a professional, polished,
  high-end desktop application, not a functional-but-rough one.

## Resolved

- **Qt license track: open-source** (LGPLv3/GPLv2), matching the
  project's own open-source, non-commercial nature. `QtPdf` inherits this
  automatically -- see "PDF, text layer" above.
- **DOCX library: in-house extractor, not DocWire.** DocToText/DocWire was
  the first option considered (explicitly proposed, not overlooked) --
  rejected after checking its actual license (GPL-2.0-only or paid
  commercial) and build requirements (vcpkg, C++20, an autotools
  bootstrap chain, wraps other libraries like `wv2` internally) in favor
  of a small in-house zip+XML extractor, on build-footprint grounds
  independent of the license question. See "DOCX" above for the full
  reasoning.
