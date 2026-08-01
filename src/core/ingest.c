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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
