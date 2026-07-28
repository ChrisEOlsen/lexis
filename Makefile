# Top-level build for the LEXIS C core (src/core/ + include/).
# Links against libsqlite3, libcurl, and pthreads per spec section 6.
# `make check` builds + runs every tests/core/test_*.c against the real
# module sources — one command, exit code reflects pass/fail.
# `make lexis` builds the CLI binary (src/core/main.c).

CFLAGS  := $(shell cat compile_flags.txt) -pedantic $(shell curl-config --cflags)
LDLIBS  := -lsqlite3 -lm $(shell curl-config --libs)
TESTDIR := tests/core
BUILD   := build

# Real (non-stub) module sources — grows as stub .c files gain content.
CORE_SRCS := src/core/tokenizer.c src/core/stopwords.c src/core/sqlite_store.c src/core/bm25.c src/core/ingest.c src/core/openrouter_client.c src/core/vendor/cJSON.c src/core/wordnet.c src/core/query_formulation.c src/core/string_builder.c src/core/generation.c src/core/lemmatizer.c src/core/query_log.c src/core/config.c

TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILD)/%,$(TEST_SRCS))

.PHONY: check clean

check: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "-- $$bin --"; \
		./$$bin || exit 1; \
	done

$(BUILD)/%: $(TESTDIR)/%.c $(CORE_SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(TESTDIR) -o $@ $< $(CORE_SRCS) $(LDLIBS)

lexis: src/core/main.c $(CORE_SRCS)
	$(CC) $(CFLAGS) -o lexis src/core/main.c $(CORE_SRCS) $(LDLIBS)

clean:
	rm -rf $(BUILD) lexis
