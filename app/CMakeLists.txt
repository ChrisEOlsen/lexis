# Qt/C++ desktop UI for LEXIS. See ../APP_SPEC.md for the design this
# scaffolds (multi-corpus groups, drag-and-drop multi-format ingestion).
#
# Entirely separate build from the top-level Makefile, which keeps
# building the CLI (`lexis`) and every test binary exactly as it does
# today -- nothing here changes that. Qt's own tooling (MOC/UIC/RCC)
# needs CMake (or the legacy qmake), which the Makefile has no reason to
# grow just for this.
#
# lexis_core (below) is a static library built from the exact same
# src/core/*.c files the Makefile's CORE_SRCS variable lists (everything
# except main.c, which owns the CLI's own entry point and has no place
# in a library). The two file lists are duplicated across build systems
# rather than unified into one -- see the Makefile's own CORE_SRCS
# comment for why that's an accepted, low-frequency bit of upkeep rather
# than a real problem.
cmake_minimum_required(VERSION 3.21)
project(lexis_app LANGUAGES C CXX)

# Feeds ../app/.clangd, which points clangd at this build directory's
# compile_commands.json for every file under app/ -- gives clangd real,
# per-file C++/Qt-aware flags for this subtree instead of falling back
# to the repo root's compile_flags.txt (written for the C core: -std=c11
# and no Qt include paths, both wrong for these files).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Same hardcoded-Homebrew-keg-path tradeoff the top-level Makefile
# already makes for postgresql@18/llama.cpp/ggml (see its own PG_CONFIG/
# LLAMA_CPP_DIR/GGML_DIR comments) -- qtbase is a Homebrew keg-only
# formula (not brew-linked into a stable path), so it needs to be named
# explicitly for find_package() to locate it. Machine-specific, not
# meant to be portable as-is, matching every other path in this project.
set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qtbase" ${CMAKE_PREFIX_PATH})

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(LEXIS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Mirrors the top-level Makefile's CORE_SRCS exactly -- see this file's
# header comment.
set(LEXIS_CORE_SRCS
    ${LEXIS_ROOT}/src/core/tokenizer.c
    ${LEXIS_ROOT}/src/core/stopwords.c
    ${LEXIS_ROOT}/src/core/pg_store.c
    ${LEXIS_ROOT}/src/core/bm25.c
    ${LEXIS_ROOT}/src/core/ingest.c
    ${LEXIS_ROOT}/src/core/local_llm_client.c
    ${LEXIS_ROOT}/src/core/vendor/cJSON.c
    ${LEXIS_ROOT}/src/core/wordnet.c
    ${LEXIS_ROOT}/src/core/query_formulation.c
    ${LEXIS_ROOT}/src/core/string_builder.c
    ${LEXIS_ROOT}/src/core/generation.c
    ${LEXIS_ROOT}/src/core/lemmatizer.c
    ${LEXIS_ROOT}/src/core/query_log.c
    ${LEXIS_ROOT}/src/core/config.c
    ${LEXIS_ROOT}/src/core/bulk_ingest.c
    ${LEXIS_ROOT}/src/core/eval.c
    ${LEXIS_ROOT}/src/core/csv_parse.c
)

add_library(lexis_core STATIC ${LEXIS_CORE_SRCS})
target_include_directories(lexis_core PUBLIC
    ${LEXIS_ROOT}/include
    ${LEXIS_ROOT}/include/vendor
)

# Same hardcoded-postgresql@18-keg tradeoff the top-level Makefile makes
# (see its own PG_CONFIG comment) -- deliberately not brew-linked, so
# plain pkg-config resolution would find the wrong version otherwise.
# Resolved via pg_config itself, not a guessed path, for the same reason
# the Makefile does: postgresql@18's actual libdir/includedir don't sit
# where a naive "/opt/homebrew/opt/postgresql@18/lib"-style guess would
# put them (confirmed directly -- pg_config --libdir reports
# /opt/homebrew/lib/postgresql@18, a sibling path, not a subdirectory of
# the opt/ keg link at all).
set(PG_CONFIG /opt/homebrew/opt/postgresql@18/bin/pg_config)
execute_process(COMMAND ${PG_CONFIG} --includedir OUTPUT_VARIABLE PG_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --libdir OUTPUT_VARIABLE PG_LIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

set(LLAMA_CPP_DIR /opt/homebrew/Cellar/llama.cpp/10180)
set(GGML_DIR /opt/homebrew/Cellar/ggml/0.18.0)

target_include_directories(lexis_core PRIVATE
    ${PG_INCLUDE_DIR}
    ${LLAMA_CPP_DIR}/include
    ${GGML_DIR}/include
)
target_link_directories(lexis_core PUBLIC ${PG_LIB_DIR} ${LLAMA_CPP_DIR}/lib ${GGML_DIR}/lib)
target_link_libraries(lexis_core PUBLIC pq llama ggml ggml-base pthread)

qt_add_executable(lexis_app
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/LexisEngine.h
    src/LexisEngine.cpp
    src/GroupSidebar.h
    src/GroupSidebar.cpp
    src/GroupContentView.h
    src/GroupContentView.cpp
    src/IngestWorker.h
    src/IngestWorker.cpp
)
target_include_directories(lexis_app PRIVATE ${PG_INCLUDE_DIR})
target_link_libraries(lexis_app PRIVATE lexis_core Qt6::Widgets)

# Needed to actually run the built binary straight out of the build
# directory during development (no install step) -- lexis_core's link
# requirements above only get the *linker* to llama.cpp/ggml at build
# time; the dynamic loader needs its own path at run time too, same
# reason the Makefile passes -Wl,-rpath for the CLI binary.
set_target_properties(lexis_app PROPERTIES
    BUILD_RPATH "${LLAMA_CPP_DIR}/lib;${GGML_DIR}/lib"
)
