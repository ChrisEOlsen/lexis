# APP_SPEC.md

Design for turning LEXIS from a single-corpus CLI tool into a Qt/C++
desktop application supporting multiple independent document collections
("groups") and multi-format ingestion (PDF, DOCX, images/scans, CSV, TXT).
This started as a target-state design document; the multi-corpus backend,
the Qt app itself, and all four format-extraction adapters described
below are now built and verified (see git history on the
`qt-ui-multicorpus` branch) -- what remains unbuilt is called out
explicitly in "Open items". `CURRENT_STATE.md` still describes only the
CLI/backend, not the Qt app.

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
| CSV | In-house, strict RFC 4180 parser (`src/core/csv_parse.c`). |
| DOCX | In-house extractor (`app/src/DocxExtractor.cpp`, pugixml + libzip). |
| PDF (text layer) | `poppler-cpp` (`app/src/PdfExtractor.cpp`) -- see below for why not `QtPdf`. |
| PDF (scanned) / images | Tesseract OCR (`app/src/OcrExtractor.cpp`) -- image files done, scanned-PDF page-render fallback not yet wired up. |

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

**DOCX.** A DOCX file is a ZIP archive containing `word/document.xml`
(plus optional `word/header*.xml`/`word/footer*.xml`/`word/footnotes.xml`/
`word/endnotes.xml`), whose `<w:t>` elements hold the actual text runs.
In-house extractor built on `pugixml` (XML parsing, MIT) and `libzip`
(zip reading, BSD) -- both real Homebrew formulas, no exotic build chain.
Walks `<w:p>` paragraphs and their `<w:t>` descendants via XPath matched
on `local-name()` (namespace-prefix-independent), joining paragraphs with
`\n`. `<w:delText>` (tracked-changes deletions) is a different element
name than `<w:t>` and is excluded automatically as a consequence, not via
special-case logic; `<w:ins>` (tracked-changes insertions) wraps ordinary
`<w:t>` content and is included, correctly, the same way. Headers/
footers/footnotes/endnotes are discovered by enumerating the archive's
entries (a document can have zero to several header/footer variants), not
guessed by filename. Verified against a fixture exercising exactly these
cases (multi-run paragraphs, a tracked deletion that must be excluded, a
tracked insertion that must survive, a table, a header part).

Considered and rejected: DocWire SDK -- technically capable (nearly 100
formats, active project, genuine OCR via a real `ocr_parser` pipeline
stage), but licensed AGPL-3.0-only or paid-commercial (corrected here --
earlier research in this project's history mis-recorded it as plain
GPL-2.0), and its vcpkg port has no feature flag to scope the build down:
installing it at all means building its *entire* dependency graph from
source -- boost, tesseract, pdfium (+ freetype/icu/libjpeg-turbo/
openjpeg), libxml2, minizip, lexbor, and `libpff`/`mailio` (PST/OST email
parsing, a format this app has no use for) -- a multi-hour, multi-
gigabyte commitment for three formats' worth of need. Also considered:
DuckX (MIT, real, 505 stars) -- built on the same two libraries
(`pugixml` + a small zip library) this project ended up using directly,
but oriented toward editing/creating DOCX files rather than read-only
bulk extraction, with unconfirmed header/footer/table support -- adopting
it wouldn't have saved the work of getting those right.

**PDF, text layer.** `poppler-cpp` (`poppler::document::load_from_file()`,
`page::text(rectf(), poppler::page::physical_layout)` for reading-order-
correct extraction on multi-column pages), a real Homebrew formula
(`brew install poppler`), no exotic build chain.

Originally planned as Qt's own `QtPdf` module instead (ships with Qt, API
confirmed real via Qt 6.8 docs: `QPdfDocument::getAllText(page)`) --
reversed after discovering `QtPdf`'s source lives inside the
`qtwebengine` repository and building it requires `gn` + `ninja`
(Chromium's own build tools, not CMake) plus Node.js, to build PDFium
from source. Not in Homebrew's `qt` formula at all (checked directly --
absent even from the 38-dependency umbrella package), almost certainly
because of exactly this build weight. This undermined the entire reason
`QtPdf` was chosen (ships with Qt, zero extra build cost) -- in practice
it would have meant a build chain comparable to or worse than DocWire's
vcpkg requirement, the same class of problem being avoided elsewhere in
this document. `poppler-cpp` was the original alternative, passed over
initially only to avoid a GPL dependency -- moot once GPL was confirmed
not a blocker for this non-commercial, open-source project.

**Images and scanned PDFs.** Tesseract OCR (Apache 2.0, real Homebrew
formula, proper C++ API -- `tesseract::TessBaseAPI::Init()`/`SetImage(Pix*)`/
`GetUTF8Text()`, image loading via its own Leptonica dependency's
`pixRead()`). Verified against a real rendered PNG with known text.
Plain image files (PNG/JPEG/TIFF/BMP) are wired up end-to-end, dropped
straight into a group like any other format.

Scanned-PDF detection is designed but **not yet implemented**: run the
text-layer extraction first (`extractPdfText()`); if it comes back empty,
that's almost certainly a scanned PDF (no text layer at all) rather than
a genuinely blank document -- currently reported to the user as "no text
found" rather than falling back to rendering pages and OCRing them. The
render step would use `poppler-cpp`'s own `page_renderer` (already a
project dependency, no new library needed) to rasterize each page, then
feed that straight into the same `extractTextFromImage()` path plain
images already use.

Flagged explicitly, not treated as solvable by better engineering: OCR is
inherently slower per page than any text-layer extraction path, by a wide
margin. No library choice removes that cost -- only parallelism (see
below) and Tesseract's own engine-mode speed/accuracy trade-off are real
levers. A corpus with a meaningful fraction of scanned documents will
take noticeably longer to ingest than an all-text-layer corpus of the
same size, and that's expected, not a bug.

## Performance: extraction off the UI thread, not yet parallelized

Target scale is thousands of documents (personal/corporate collections),
not MS MARCO's millions -- this shapes the rebuild-on-append and
non-segmented decisions above, and shapes this too.

**As built**, format extraction (CSV/DOCX/PDF/OCR) runs serially, one
dropped file at a time, inside `IngestWorker`'s single background
`QThread` -- entirely off the UI thread (critical: OCR specifically is
slow enough that running it inline in the drop handler, before the
worker even starts, would freeze the UI exactly the way the worker
exists to prevent for the database rebuild itself), but not parallelized
across files the way `bulk_ingest.c`'s Phase 2 worker pool parallelizes
chunking/tokenizing once extraction hands it plain text. Once every
dropped file's text is in hand, `bulk_ingest_rebuild_corpus()` takes over
and inherits all of that pipeline's existing multi-threaded throughput,
unchanged.

**Not yet done:** parallelizing extraction itself across multiple worker
threads (one thread per dropped file, up to some concurrency cap) the way
originally envisioned here. Worth adding if real usage shows a multi-file
drop (especially a batch of scans) taking noticeably longer serially than
it would in parallel; not required for a working v1 at this app's target
scale.

## Explicitly out of scope for this spec

- Per-column CSV field selection / structured metadata UI.
- Incremental (non-rebuild) small-batch append fast path.
- Segment-based indexing (Lucene-style, with query-time stat merging).
- Cross-group / multi-group simultaneous search.

## Open items

- Scanned-PDF OCR fallback -- see "Images and scanned PDFs" above.
  Currently a scanned PDF is reported to the user as "no text found",
  not automatically rendered and OCR'd.
- Extraction parallelism across multiple dropped files -- see
  "Performance" above. Currently serial within one background thread.
- UI/UX specifics beyond "drag files into a group, switch active group" --
  left to implementation judgment; the bar is a professional, polished,
  high-end desktop application, not a functional-but-rough one.

## Resolved

- **DOCX: in-house extractor (`pugixml` + `libzip`), not DocWire.**
  DocWire was the first option considered (explicitly proposed, not
  overlooked) -- rejected on build-footprint grounds (its vcpkg port
  builds its entire ~100-format dependency graph regardless of which
  formats are actually needed) independent of licensing. See "DOCX"
  above for the full reasoning, including DuckX (a real alternative
  considered and also passed over).
- **PDF: `poppler-cpp`, not `QtPdf`.** Reversed after discovering
  `QtPdf` requires Chromium's own `gn`/`ninja`/Node.js build chain for
  PDFium, not a normal CMake build, and isn't in Homebrew at all -- see
  "PDF, text layer" above for the full story.
- **OCR: Tesseract**, as originally planned -- the one piece of this
  whole format-adapter question that never needed reconsidering; real
  Homebrew formula, no exotic build chain, and no lighter alternative
  exists for OCR specifically (confirmed by research, not assumed --
  OCR isn't a "small library" problem the way DOCX/CSV parsing are).
- **Qt license track: open-source** (LGPLv3/GPLv2), matching the
  project's own open-source, non-commercial nature. Moot for `QtPdf`
  specifically now that PDF uses `poppler-cpp` instead, but still the
  live license track for Qt itself.
