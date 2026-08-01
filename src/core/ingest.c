/*
 * Implementation of the ingestion pipeline.
 * See include/ingest.h for the module's role (spec 5.2.1, Stages 1-2).
 */

/* See tokenizer.c for why this must come before any #include (strdup and
 * strtok_r are POSIX extensions hidden by glibc under strict -std=c11
 * otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "ingest.h"
#include "tokenizer.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *ingest_read_file(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "ingest_read_file: could not open file");
    return NULL;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fprintf(stderr, "ingest_read_file: could not fseek file");
    fclose(fp);
    return NULL;
  }

  long file_size = ftell(fp);
  if (file_size == -1) {
    fclose(fp);
    return NULL;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    fprintf(stderr, "ingest_read_file: could not fseek file");
    fclose(fp);
    return NULL;
  }

  char *buffer = malloc(file_size + 1);
  if (buffer == NULL) {
    fclose(fp);
    return NULL;
  }

  long bytes_read = fread(buffer, 1, file_size, fp);
  if (bytes_read != file_size) {
    free(buffer);
    fclose(fp);
    return NULL;
  }

  buffer[file_size] = '\0';
  fclose(fp);
  return buffer;
}

TokenList *ingest_split_words(const char *text) {
  char *mut_cpy = strdup(text);
  if (mut_cpy == NULL) {
    fprintf(stderr, "ingest_split_words: strdup failed.");
    return NULL;
  }

  TokenList *list = token_list_create();
  if (list == NULL) {
    fprintf(stderr, "ingest_split_words: token_list_create failed.");
    free(mut_cpy);
    return NULL;
  }

  char *saveptr;
  char *word = strtok_r(mut_cpy, " \t\r\n", &saveptr);
  while (word != NULL) {
    if (token_list_append(list, word) != 0) {
      token_list_free(list);
      free(mut_cpy);
      return NULL;
    }

    word = strtok_r(NULL, " \t\r\n", &saveptr);
  }

  free(mut_cpy);

  return list;
}

char *ingest_join_words(const TokenList *words, size_t start, size_t end) {
  size_t total_len = 0;
  for (size_t i = start; i < end; i++) {
    total_len += strlen(words->terms[i]);
    if (i < end - 1)
      total_len++;
  }

  char *buffer = malloc(total_len + 1);
  if (buffer == NULL) {
    fprintf(stderr, "ingest_join_words: malloc failed");
    return NULL;
  }

  char *write_pos = buffer;
  for (size_t i = start; i < end; i++) {
    size_t word_len = strlen(words->terms[i]);
    memcpy(write_pos, words->terms[i], word_len);
    write_pos += word_len;
    if (i < end - 1) {
      *write_pos = ' ';
      write_pos += 1;
    }
  }

  *write_pos = '\0';
  return buffer;
}

TokenList *ingest_chunk_words(const TokenList *words, size_t chunk_size,
                              size_t overlap) {
  if (chunk_size == 0 || overlap >= chunk_size) {
    return NULL;
  }
  TokenList *chunks = token_list_create();
  if (chunks == NULL)
    return NULL;

  if (words->count == 0)
    return chunks;

  size_t step = chunk_size - overlap;
  size_t start = 0;

  while (1) {
    size_t end = start + chunk_size;
    if (end > words->count)
      end = words->count;

    char *chunk_text = ingest_join_words(words, start, end);

    if (chunk_text == NULL) {
      token_list_free(chunks);
      return NULL;
    }

    if (token_list_append(chunks, chunk_text) != 0) {
      free(chunk_text);
      token_list_free(chunks);
      return NULL;
    }

    free(chunk_text);

    if (end == words->count)
      break;

    start = start + step;
  }

  return chunks;
}

TokenList *ingest_lemmatize_terms(const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                   const TokenList *terms) {
  TokenList *lemmas = token_list_create();
  if (lemmas == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < terms->count; i++) {
    char *lemma = lemmatize(lemmatizer, wordnet, terms->terms[i]);
    if (lemma == NULL) {
      token_list_free(lemmas);
      return NULL;
    }
    int appended = token_list_append(lemmas, lemma);
    free(lemma);
    if (appended != 0) {
      token_list_free(lemmas);
      return NULL;
    }
  }

  return lemmas;
}

int ingest_count_distinct_terms(const TokenList *terms, const char ***distinct_terms_out,
                                 int **frequencies_out, size_t *distinct_count_out) {
  if (terms->count == 0) {
    *distinct_terms_out = NULL;
    *frequencies_out = NULL;
    *distinct_count_out = 0;
    return 0;
  }

  const char **distinct_terms = malloc(sizeof(char *) * terms->count);
  int *frequencies = malloc(sizeof(int) * terms->count);
  if (distinct_terms == NULL || frequencies == NULL) {
    free(distinct_terms);
    free(frequencies);
    return -1;
  }

  size_t distinct_count = 0;
  for (size_t i = 0; i < terms->count; i++) {
    int already_indexed = 0;
    for (size_t j = 0; j < i; j++) {
      if (strcmp(terms->terms[j], terms->terms[i]) == 0) {
        already_indexed = 1;
        break;
      }
    }
    if (already_indexed) {
      continue;
    }

    int frequency = 0;
    for (size_t j = i; j < terms->count; j++) {
      if (strcmp(terms->terms[j], terms->terms[i]) == 0) {
        frequency++;
      }
    }

    distinct_terms[distinct_count] = terms->terms[i];
    frequencies[distinct_count] = frequency;
    distinct_count++;
  }

  *distinct_terms_out = distinct_terms;
  *frequencies_out = frequencies;
  *distinct_count_out = distinct_count;
  return 0;
}

/* Indexes one chunk's already-tokenized, already-stopword-filtered,
 * already-lemmatized term list against `passage_id`: resolves each
 * distinct term (see ingest_count_distinct_terms()) to a terms.id and
 * writes exactly one posting row for it.
 *
 * Batches the whole chunk into pg_store_get_or_create_terms() +
 * pg_store_insert_postings() -- a fixed handful of round trips regardless
 * of how many distinct terms the chunk has, instead of two round trips
 * *per term*. That per-term round-trip cost, at ~0.13ms each measured
 * against this project's Docker Postgres, dominated single-threaded
 * ingestion latency badly enough to make it slower than the SQLite
 * version it replaced (see LIMITATIONS.md) -- SQLite's in-process calls
 * never paid a network round trip at all. `terms->count` (the full,
 * non-distinct lemma count) doubles as this passage's token_count --
 * the same value already passed to pg_store_insert_passage() -- and gets
 * denormalized onto every posting row here too (see pg_store.c's schema
 * comment for why). Returns 0 on success, -1 on a database error. */
static int ingest_index_chunk_terms(PgStore *store, const TokenList *terms,
                                    int64_t passage_id, TermCache *cache,
                                    TermCachePending *pending) {
  if (terms->count == 0) {
    return 0;
  }

  const char **distinct_terms;
  int *frequencies;
  size_t distinct_count;
  if (ingest_count_distinct_terms(terms, &distinct_terms, &frequencies, &distinct_count) != 0) {
    return -1;
  }

  int64_t *term_ids =
      (cache != NULL)
          ? term_cache_get_or_create_terms(cache, pending, store, distinct_terms, distinct_count)
          : pg_store_get_or_create_terms(store, distinct_terms, distinct_count);
  free(distinct_terms);
  if (term_ids == NULL) {
    free(frequencies);
    return -1;
  }

  int result = pg_store_insert_postings(store, term_ids, passage_id, frequencies, (int)terms->count,
                                        distinct_count);
  free(term_ids);
  free(frequencies);
  return result;
}

/* The real per-document work -- chunking, tokenizing, lemmatizing,
 * persisting each chunk's passage and postings -- WITHOUT managing any
 * transaction or the term cache pending lifecycle. The caller must
 * already have an open transaction (or be inside a savepoint) and is
 * responsible for rolling back whatever that implies on failure, and for
 * committing/discarding `pending` itself. Shared core both
 * ingest_document_from_text() (one document, one transaction) and
 * ingest_document_from_text_in_batch() (many documents, one shared
 * transaction, isolated via savepoints) build on. Returns the number of
 * passages ingested (>= 0) on success, or -1 on failure. */
static long ingest_document_body(PgStore *store, const StopwordSet *stopwords,
                                  const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                  const char *text, const char *document_name,
                                  size_t chunk_size, size_t overlap, TermCache *cache,
                                  TermCachePending *pending) {
  TokenList *words = ingest_split_words(text);
  if (words == NULL) {
    return -1;
  }

  TokenList *chunks = ingest_chunk_words(words, chunk_size, overlap);
  token_list_free(words);
  if (chunks == NULL) {
    return -1;
  }

  long passages_ingested = 0;
  for (size_t i = 0; i < chunks->count; i++) {
    const char *chunk_text = chunks->terms[i];

    TokenList *terms = tokenize(chunk_text);
    if (terms == NULL) {
      token_list_free(chunks);
      return -1;
    }
    stopwords_filter(terms, stopwords);

    TokenList *lemmas = ingest_lemmatize_terms(wordnet, lemmatizer, terms);
    token_list_free(terms);
    if (lemmas == NULL) {
      token_list_free(chunks);
      return -1;
    }

    int64_t passage_id = pg_store_insert_passage(
        store, document_name, (int)i, chunk_text, (int)lemmas->count);
    if (passage_id == -1) {
      token_list_free(lemmas);
      token_list_free(chunks);
      return -1;
    }

    if (ingest_index_chunk_terms(store, lemmas, passage_id, cache, pending) != 0) {
      token_list_free(lemmas);
      token_list_free(chunks);
      return -1;
    }

    token_list_free(lemmas);
    passages_ingested++;
  }
  token_list_free(chunks);
  return passages_ingested;
}

long ingest_document_from_text(PgStore *store, const StopwordSet *stopwords,
                               const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                               const char *text, const char *document_name,
                               size_t chunk_size, size_t overlap, TermCache *cache) {
  /* Newly resolved terms stay document-local until this document's
   * transaction actually commits -- see TermCachePending's doc comment
   * for why writing them straight into the shared cache is unsafe (a
   * verified, real bug: a term "created" inside a transaction that later
   * rolls back never actually persists in Postgres, but the shared cache
   * would still claim it does, poisoning every future document that
   * uses it with a term_id that fails postings' foreign key
   * constraint). NULL when cache is NULL (the uncached path). */
  TermCachePending *pending = (cache != NULL) ? term_cache_pending_create() : NULL;
  if (cache != NULL && pending == NULL) {
    return -1;
  }

  /* One transaction for the whole document, instead of an implicit
   * (auto-committing, fsync-per-statement) transaction for every single
   * passage/term/posting INSERT -- both a large latency win (see
   * LIMITATIONS.md for measured numbers) and real atomicity: a failure
   * partway through rolls back this document's writes entirely, rather
   * than leaving it half-indexed. */
  if (pg_store_begin_transaction(store) != 0) {
    term_cache_pending_free(pending);
    return -1;
  }

  long passages_ingested = ingest_document_body(store, stopwords, wordnet, lemmatizer, text,
                                                 document_name, chunk_size, overlap, cache, pending);
  if (passages_ingested < 0) {
    pg_store_rollback_transaction(store);
    term_cache_pending_free(pending);
    return -1;
  }

  if (pg_store_commit_transaction(store) != 0) {
    term_cache_pending_free(pending);
    return -1;
  }

  /* Only now, with the transaction actually durable, fold any newly
   * resolved terms into the shared cache -- safe for every other worker
   * thread to see from this point on. No-ops safely if cache/pending are
   * both NULL (the uncached path). */
  term_cache_commit_pending(cache, pending);
  return passages_ingested;
}

long ingest_document(PgStore *store, const StopwordSet *stopwords,
                     const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                     const char *path, const char *document_name,
                     size_t chunk_size, size_t overlap, TermCache *cache) {
  char *text = ingest_read_file(path);
  if (text == NULL) {
    return -1;
  }

  long result = ingest_document_from_text(store, stopwords, wordnet, lemmatizer, text,
                                          document_name, chunk_size, overlap, cache);
  free(text);
  return result;
}

long ingest_corpus(PgStore *store, const StopwordSet *stopwords,
                   const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                   const char *dir_path, size_t chunk_size, size_t overlap) {
  DIR *dir = opendir(dir_path);
  if (dir == NULL) {
    fprintf(stderr, "ingest_corpus: could not open directory %s\n", dir_path);
    return -1;
  }

  long total_passages = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
      /* Not a regular file (a subdirectory, symlink, device, etc.)
       * -- skip it silently rather than trying to ingest it. */
      continue;
    }

    long passages = ingest_document(store, stopwords, wordnet, lemmatizer, full_path,
                                    entry->d_name, chunk_size, overlap, NULL);
    if (passages < 0) {
      fprintf(stderr, "ingest_corpus: failed to ingest %s, skipping\n",
              full_path);
      continue;
    }

    total_passages += passages;
  }

  closedir(dir);
  return total_passages;
}
