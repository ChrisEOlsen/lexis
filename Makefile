# Top-level build for the LEXIS C core (src/core/ + include/).
# Links against libpq (Postgres, experiment/postgres-migration branch --
# see LIMITATIONS.md for the sqlite-vs-postgres history), libcurl, and
# pthreads per spec section 6.
# `make check` builds + runs every tests/core/test_*.c against the real
# module sources — one command, exit code reflects pass/fail. Requires
# `docker compose up -d` running (see docker-compose.yml).
# `make lexis` builds the CLI binary (src/core/main.c).

# Hardcoded to the postgresql@18 keg specifically -- deliberately not
# `brew link`ed (would shadow whatever postgresql version is already on
# PATH system-wide), so plain `pg_config` would resolve to the wrong
# version here. Machine-specific path, same tradeoff compile_flags.txt
# already makes; fine for this experimental branch, not meant to be
# portable as-is.
PG_CONFIG := /opt/homebrew/opt/postgresql@18/bin/pg_config

CFLAGS  := $(shell cat compile_flags.txt) -pedantic $(shell curl-config --cflags)
LDLIBS  := -L$(shell $(PG_CONFIG) --libdir) -lpq -lm -lpthread $(shell curl-config --libs)
TESTDIR := tests/core
BUILD   := build

# Real (non-stub) module sources — grows as stub .c files gain content.
CORE_SRCS := src/core/tokenizer.c src/core/stopwords.c src/core/pg_store.c src/core/bm25.c src/core/ingest.c src/core/openrouter_client.c src/core/vendor/cJSON.c src/core/wordnet.c src/core/query_formulation.c src/core/string_builder.c src/core/generation.c src/core/lemmatizer.c src/core/query_log.c src/core/config.c src/core/concurrent_ingest.c

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
