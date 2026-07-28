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

/* Lemmatizes every term in `terms` (e.g. "nicknamed" -> "nickname") into a
 * freshly allocated TokenList, so the index stores the same base forms
 * query_formulation.c looks words up under at query time. Returns NULL on
 * allocation failure. */
static TokenList *ingest_lemmatize_terms(const WordNetTable *wordnet,
                                          const Lemmatizer *lemmatizer,
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

/* Indexes one chunk's already-tokenized, already-stopword-filtered,
 * already-lemmatized term list against `passage_id`: for each distinct
 * term, counts how many times it occurs in this chunk and writes exactly
 * one posting row for it. O(n^2) over the chunk's term count -- acceptable
 * at chunk scale (a few hundred terms at most), same tradeoff already made
 * for bm25_result_set_add's linear scan. Returns 0 on success, -1 on a
 * database error. */
static int ingest_index_chunk_terms(SqliteStore *store, const TokenList *terms,
                                    sqlite3_int64 passage_id) {
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

    sqlite3_int64 term_id =
        sqlite_store_get_or_create_term(store, terms->terms[i]);
    if (term_id == -1) {
      return -1;
    }

    if (sqlite_store_insert_posting(store, term_id, passage_id, frequency) !=
        0) {
      return -1;
    }
  }

  return 0;
}

long ingest_document(SqliteStore *store, const StopwordSet *stopwords,
                     const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                     const char *path, const char *document_name,
                     size_t chunk_size, size_t overlap) {
  char *text = ingest_read_file(path);
  if (text == NULL) {
    return -1;
  }

  TokenList *words = ingest_split_words(text);
  free(text);
  if (words == NULL) {
    return -1;
  }

  TokenList *chunks = ingest_chunk_words(words, chunk_size, overlap);
  token_list_free(words);
  if (chunks == NULL) {
    return -1;
  }

  /* One transaction for the whole document, instead of an implicit
   * (auto-committing, fsync-per-statement) transaction for every single
   * passage/term/posting INSERT -- both a large latency win (see
   * LIMITATIONS.md for measured numbers) and real atomicity: a failure
   * partway through rolls back this document's writes entirely, rather
   * than leaving it half-indexed. */
  if (sqlite_store_begin_transaction(store) != 0) {
    token_list_free(chunks);
    return -1;
  }

  long passages_ingested = 0;
  for (size_t i = 0; i < chunks->count; i++) {
    const char *chunk_text = chunks->terms[i];

    TokenList *terms = tokenize(chunk_text);
    if (terms == NULL) {
      sqlite_store_rollback_transaction(store);
      token_list_free(chunks);
      return -1;
    }
    stopwords_filter(terms, stopwords);

    TokenList *lemmas = ingest_lemmatize_terms(wordnet, lemmatizer, terms);
    token_list_free(terms);
    if (lemmas == NULL) {
      sqlite_store_rollback_transaction(store);
      token_list_free(chunks);
      return -1;
    }

    sqlite3_int64 passage_id = sqlite_store_insert_passage(
        store, document_name, (int)i, chunk_text, (int)lemmas->count);
    if (passage_id == -1) {
      token_list_free(lemmas);
      sqlite_store_rollback_transaction(store);
      token_list_free(chunks);
      return -1;
    }

    if (ingest_index_chunk_terms(store, lemmas, passage_id) != 0) {
      token_list_free(lemmas);
      sqlite_store_rollback_transaction(store);
      token_list_free(chunks);
      return -1;
    }

    token_list_free(lemmas);
    passages_ingested++;
  }
  token_list_free(chunks);

  if (sqlite_store_commit_transaction(store) != 0) {
    return -1;
  }
  return passages_ingested;
}

long ingest_corpus(SqliteStore *store, const StopwordSet *stopwords,
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
                                    entry->d_name, chunk_size, overlap);
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
