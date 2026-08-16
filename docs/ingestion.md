# Ingestion

How documents become searchable.

## In the app

Drop files onto a group. Supported: PDF (poppler), DOCX (built-in
extractor), plain text, and images/scans (tesseract OCR). Each file's
text is:

1. Split into overlapping chunks of about 200 words (40-word overlap,
   so a sentence living on a chunk boundary appears whole in at least
   one chunk).
2. Tokenized, filler words dropped, every word reduced to its base
   form -- the same processing questions get, so they match.
3. Written to the group's own database schema: the passage text plus
   one posting row per distinct word.

Groups are isolated: each has its own index, documents, and chats.
Ingestion runs on a background thread; the app stays usable.

## From the command line

`./lexis bulk-ingest <file.tsv>` indexes a large corpus fast. The
input is one document per line: `id<TAB>text`, RFC-4180 CSV-quoted
(plain TSV breaks on text containing backslashes or quotes).

It runs in three phases designed around one fact: parallel writers
fighting over the shared terms table was the source of every deadlock
this project ever measured.

1. **Raw append** -- one COPY streams the whole file into an unlogged
   staging table.
2. **Parallel processing** -- six worker threads chunk and tokenize,
   writing passages and staging postings *by term text*. No worker
   ever touches the terms table.
3. **Finalize** -- a single writer resolves all distinct terms at
   once and moves the staged postings into place, with the heavy
   indexes dropped for the duration and rebuilt after.

Measured throughput: ~9,000 passages/second on an M-series Mac. The
full 8.8M-passage MS MARCO corpus indexes in about 16 minutes.

Note: bulk-ingest appends. Re-indexing the same corpus twice
duplicates it -- truncate first (see the ops notes in the dev docs).

## What ingestion never does

No model calls. Indexing is pure text processing, which is why it is
fast and why a thousand-document corpus is minutes, not hours. All
model work happens at question time.
