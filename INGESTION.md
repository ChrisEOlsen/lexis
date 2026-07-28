# Ingestion Pipeline — Function Reference

Every function involved in turning a raw document into searchable database rows, listed in the order they're used. What each one does, not how.

## tokenizer.c — building blocks + text normalization

- `token_list_create` creates an empty growable list of strings — needed as the reusable container used for term lists, chunk lists, and raw word lists throughout ingestion.
- `token_list_append` adds one string to a growable list — needed to build up those lists one item at a time as they're discovered.
- `token_list_free` frees a growable list and everything in it — needed to release memory once a list of terms/words/chunks is no longer needed.
- `tokenize` splits normalized text into a clean list of lowercase, punctuation-free terms — needed to turn each chunk's raw text into the actual term list that gets indexed.

## stopwords.c — filtering out meaningless words

- `stopword_set_load` loads the stopword list from disk into memory — needed once so filtering has something to check words against.
- `stopword_set_free` frees a loaded stopword list — needed to release memory when it's no longer needed.
- `stopword_set_contains` checks whether one word is a stopword — needed internally by the filtering step.
- `stopwords_filter` removes every stopword from a term list — needed to strip meaningless words (the, is, a, of...) before they ever reach the index.

## sqlite_store.c — the database write path

- `sqlite_store_open` opens (or creates) the database file and its tables — needed before anything can be read from or written to the index.
- `sqlite_store_close` closes the database connection — needed to cleanly release the file when done.
- `sqlite_store_insert_passage` saves one chunk's raw text into the database — needed so passages can later be retrieved and shown to a user or an LLM.
- `sqlite_store_get_or_create_term` looks up a term's id, creating one if it's never been seen — needed so every term has one consistent small integer id instead of repeating the string everywhere.
- `sqlite_store_insert_posting` records that a term occurs N times in a passage — needed to actually build the inverted index.

## ingest.c — reading, chunking, and orchestration

- `ingest_read_file` loads a document's full text from disk into memory — needed as the very first step before anything else can happen.
- `ingest_split_words` splits raw text into whitespace-separated words, punctuation intact — needed as the raw material chunking works with, before any normalization.
- `ingest_join_words` reconstructs a range of words back into a single string — needed to turn one window of words into one chunk's actual stored text.
- `ingest_chunk_words` groups a document's words into overlapping windows — needed to break a long document into passage-sized pieces, with configurable size and overlap.
- `ingest_index_chunk_terms` (internal helper) counts each term's frequency within one chunk and writes its posting — needed so a repeated term collapses into one accurate frequency count instead of duplicate entries.
- `ingest_document` runs one file through the entire pipeline — read, chunk, tokenize, filter, persist — needed to turn a single document into passages and postings in the database.
- `ingest_corpus` runs every file in a directory through `ingest_document` — needed to ingest a whole corpus in one call instead of one file at a time.
