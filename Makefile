# Top-level build for the LEXIS C core (src/core/ + include/).
# Links against libpq (Postgres, experiment/postgres-migration branch --
# see LIMITATIONS.md for the sqlite-vs-postgres history), llama.cpp/ggml
# (local LLM inference -- see LIMITATIONS.md for the OpenRouter-to-local
# history), and pthreads per spec section 6.
#
# Everything -- `make check`'s test suite (database lexis_test) and
# `make lexis`'s real CLI (database lexis) -- targets the same native
# Homebrew postgresql@18 install (port 5434), via `make pg-start`/`make
# pg-stop`; it does not auto-start on login. A separate Docker Postgres
# instance was used for tests earlier in this project (Docker Desktop's
# macOS VM networking layer adds real per-round-trip latency a native
# install doesn't pay, which was already the reason the real CLI moved
# off it) -- removed once the test suite was verified passing against
# native Postgres too, so there was no longer a reason to run two
# separate Postgres processes for one project. Entirely separate from
# this machine's pre-existing postgresql@14 (port 5432, unrelated
# projects) -- never touch that.

# Hardcoded to the postgresql@18 keg specifically -- deliberately not
# `brew link`ed (would shadow whatever postgresql version is already on
# PATH system-wide), so plain `pg_config` would resolve to the wrong
# version here. Machine-specific path, same tradeoff compile_flags.txt
# already makes; fine for this experimental branch, not meant to be
# portable as-is.
PG_CONFIG := /opt/homebrew/opt/postgresql@18/bin/pg_config

# Hardcoded to the exact installed Cellar versions, same tradeoff as
# PG_CONFIG above -- `brew` doesn't put these on a stable, version-free
# path, and this is an experimental branch, not meant to be portable as-is.
LLAMA_CPP_DIR := /opt/homebrew/Cellar/llama.cpp/10180
GGML_DIR := /opt/homebrew/Cellar/ggml/0.18.0

CFLAGS  := $(shell cat compile_flags.txt) -pedantic
LDLIBS  := -L$(shell $(PG_CONFIG) --libdir) -lpq -lm -lpthread \
           -L$(LLAMA_CPP_DIR)/lib -L$(GGML_DIR)/lib -lllama -lggml -lggml-base \
           -Wl,-rpath,$(LLAMA_CPP_DIR)/lib -Wl,-rpath,$(GGML_DIR)/lib
TESTDIR := tests/core
BUILD   := build

# Real (non-stub) module sources — grows as stub .c files gain content.
CORE_SRCS := src/core/tokenizer.c src/core/stopwords.c src/core/pg_store.c src/core/bm25.c src/core/ingest.c src/core/local_llm_client.c src/core/vendor/cJSON.c src/core/wordnet.c src/core/query_formulation.c src/core/string_builder.c src/core/generation.c src/core/lemmatizer.c src/core/query_log.c src/core/config.c src/core/bulk_ingest.c src/core/eval.c src/core/csv_parse.c src/core/tool_router.c

TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILD)/%,$(TEST_SRCS))

# Native Postgres data directory (port 5434) -- see LEXIS_DB_CONNINFO in
# main.c. Auto-initialized by `brew install postgresql@18`; this just
# starts/stops it.
PG_NATIVE_BIN  := /opt/homebrew/opt/postgresql@18/bin
PG_NATIVE_DATA := /opt/homebrew/var/postgresql@18

.PHONY: check clean pg-start pg-stop

pg-start:
	$(PG_NATIVE_BIN)/pg_ctl -D $(PG_NATIVE_DATA) -l $(PG_NATIVE_DATA)/server.log start

pg-stop:
	$(PG_NATIVE_BIN)/pg_ctl -D $(PG_NATIVE_DATA) stop

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
