# LEXIS C core build. Tests (lexis_test) and CLI (lexis) share native postgresql@18 on port 5434.
# Never touch postgresql@14 on 5432 (unrelated projects).

# Local overrides live in config/local.mk (gitignored; see local.mk.example).
-include config/local.mk

# Pinned to the postgresql@18 keg (not brew-linked); ?= lets env/local.mk win.
PG_CONFIG ?= /opt/homebrew/opt/postgresql@18/bin/pg_config

# Pinned Cellar versions; same non-portable tradeoff as PG_CONFIG.
LLAMA_CPP_DIR ?= /opt/homebrew/Cellar/llama.cpp/10360
GGML_DIR ?= /opt/homebrew/Cellar/ggml/0.19.0

CFLAGS  := $(shell cat compile_flags.txt) -pedantic
# -lc++ for jinja_chat_template.cpp, the one C++ unit (minja needs C++17).
LDLIBS  := -L$(shell $(PG_CONFIG) --libdir) -lpq -lm -lpthread \
           -L$(LLAMA_CPP_DIR)/lib -L$(GGML_DIR)/lib -lllama -lggml -lggml-base \
           -Wl,-rpath,$(LLAMA_CPP_DIR)/lib -Wl,-rpath,$(GGML_DIR)/lib -lc++
TESTDIR := tests/core
BUILD   := build

# Single core source list, shared with app/CMakeLists.txt (same file).
CORE_SRCS_FILE := src/core/sources.txt
CORE_SRCS := $(shell cat $(CORE_SRCS_FILE))

# Sole C++ unit; built separately with $(CXX)/$(CXXFLAGS), linked as an object.
JINJA_SRC := src/core/jinja_chat_template.cpp
JINJA_OBJ := $(BUILD)/jinja_chat_template.o
CXX      := c++
CXXFLAGS := -std=c++17 -Wall -Wextra -Iinclude -Isrc/core/vendor

TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)
# test_stream_identity needs the ~5GB model; excluded from `make check`, manual target only.
TEST_SRCS := $(filter-out $(TESTDIR)/test_stream_identity.c,$(TEST_SRCS))
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILD)/%,$(TEST_SRCS))
STREAM_IDENTITY_BIN := $(BUILD)/test_stream_identity

# Native Postgres (port 5434); see LEXIS_DB_CONNINFO in main.c.
PG_NATIVE_BIN  ?= /opt/homebrew/opt/postgresql@18/bin
PG_NATIVE_DATA ?= /opt/homebrew/var/postgresql@18

.PHONY: check clean pg-start pg-stop stream-identity

# F3 tripwire (see dev/UI_UPGRADES_SPEC.md): streaming must match plain output.
# Run by hand whenever local_llm_client.c's decode loop changes.
stream-identity: $(STREAM_IDENTITY_BIN)
	./$(STREAM_IDENTITY_BIN)

$(STREAM_IDENTITY_BIN): $(TESTDIR)/test_stream_identity.c $(CORE_SRCS) $(JINJA_OBJ) $(CORE_SRCS_FILE)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_SRCS) $(JINJA_OBJ) $(LDLIBS)

pg-start:
	$(PG_NATIVE_BIN)/pg_ctl -D $(PG_NATIVE_DATA) -l $(PG_NATIVE_DATA)/server.log start

pg-stop:
	$(PG_NATIVE_BIN)/pg_ctl -D $(PG_NATIVE_DATA) stop

check: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "-- $$bin --"; \
		./$$bin || exit 1; \
	done

$(JINJA_OBJ): $(JINJA_SRC)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%: $(TESTDIR)/%.c $(CORE_SRCS) $(JINJA_OBJ) $(CORE_SRCS_FILE)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(TESTDIR) -o $@ $< $(CORE_SRCS) $(JINJA_OBJ) $(LDLIBS)

lexis: src/core/main.c $(CORE_SRCS) $(JINJA_OBJ) $(CORE_SRCS_FILE)
	$(CC) $(CFLAGS) -o lexis src/core/main.c $(CORE_SRCS) $(JINJA_OBJ) $(LDLIBS)

clean:
	rm -rf $(BUILD) lexis
