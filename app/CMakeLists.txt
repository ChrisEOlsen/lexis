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
# LLAMA_CPP_DIR/GGML_DIR comments) -- qtbase and qtdeclarative (Qt
# Quick/QML -- a separate Homebrew keg from qtbase) are both keg-only
# formulas (not brew-linked into a stable path), so both need to be
# named explicitly for find_package() to locate them. Machine-specific,
# not meant to be portable as-is, matching every other path in this
# project.
set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qtbase" "/opt/homebrew/opt/qtdeclarative" ${CMAKE_PREFIX_PATH})

# find_package(Qt6 COMPONENTS ...) resolves every component relative to
# wherever it found Qt6Config.cmake itself (qtbase's prefix) rather than
# searching all of CMAKE_PREFIX_PATH per component -- confirmed directly
# (it failed even with qtdeclarative's prefix already on the path
# above). Pointing these two *_DIR variables at qtdeclarative's own
# package config directories directly is what actually works.
set(Qt6Quick_DIR "/opt/homebrew/opt/qtdeclarative/lib/cmake/Qt6Quick")
set(Qt6QuickControls2_DIR "/opt/homebrew/opt/qtdeclarative/lib/cmake/Qt6QuickControls2")

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)
qt_standard_project_setup()

# Format-extraction adapters (see ../APP_SPEC.md's "Document ingestion"
# section): poppler-cpp for PDF, pugixml + libzip for an in-house DOCX
# extractor, tesseract (+ leptonica, its image-loading dependency) for
# OCR. All four ship real pkg-config files, unlike postgresql@18/
# llama.cpp/ggml above -- PkgConfig::<name> IMPORTED targets carry their
# own include/link flags, no manual path-guessing needed for these.
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)
pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
pkg_check_modules(TESSERACT REQUIRED IMPORTED_TARGET tesseract)
pkg_check_modules(LEPTONICA REQUIRED IMPORTED_TARGET lept)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)

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
    ${LEXIS_ROOT}/src/core/synonym_table.c
    ${LEXIS_ROOT}/src/core/reranker.c
    ${LEXIS_ROOT}/src/core/query_formulation.c
    ${LEXIS_ROOT}/src/core/retrieval.c
    ${LEXIS_ROOT}/src/core/string_builder.c
    ${LEXIS_ROOT}/src/core/generation.c
    ${LEXIS_ROOT}/src/core/lemmatizer.c
    ${LEXIS_ROOT}/src/core/query_log.c
    ${LEXIS_ROOT}/src/core/config.c
    ${LEXIS_ROOT}/src/core/bulk_ingest.c
    ${LEXIS_ROOT}/src/core/eval.c
    ${LEXIS_ROOT}/src/core/csv_parse.c
    ${LEXIS_ROOT}/src/core/tool_router.c
    ${LEXIS_ROOT}/src/core/corpus_summary.c
    ${LEXIS_ROOT}/src/core/jinja_chat_template.cpp
)

add_library(lexis_core STATIC ${LEXIS_CORE_SRCS})
target_include_directories(lexis_core PUBLIC
    ${LEXIS_ROOT}/include
    ${LEXIS_ROOT}/include/vendor
)
# src/core/vendor also holds the vendored minja/nlohmann-json headers
# jinja_chat_template.cpp needs (real Jinja2 chat-template rendering for
# models too sophisticated for llama_chat_apply_template()'s plain
# built-in matcher -- see local_llm_client.c's own comment). PRIVATE, not
# PUBLIC like the two above -- nothing outside this one C++ translation
# unit needs to see minja/nlohmann directly.
target_include_directories(lexis_core PRIVATE
    ${LEXIS_ROOT}/src/core/vendor
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

set(LLAMA_CPP_DIR /opt/homebrew/Cellar/llama.cpp/10360)
set(GGML_DIR /opt/homebrew/Cellar/ggml/0.19.0)

# tesseract's language data (tessdata) lives under its own Cellar
# version path, not a stable /opt/homebrew/share/ location -- resolved
# via `brew --prefix` rather than hardcoding the version number, same
# reasoning as PG_CONFIG's resolution above.
execute_process(COMMAND brew --prefix tesseract OUTPUT_VARIABLE TESSERACT_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
set(LEXIS_TESSDATA_PREFIX "${TESSERACT_PREFIX}/share/tessdata")

target_include_directories(lexis_core PRIVATE
    ${PG_INCLUDE_DIR}
    ${LLAMA_CPP_DIR}/include
    ${GGML_DIR}/include
)
target_link_directories(lexis_core PUBLIC ${PG_LIB_DIR} ${LLAMA_CPP_DIR}/lib ${GGML_DIR}/lib)
target_link_libraries(lexis_core PUBLIC pq llama ggml ggml-base pthread)

qt_add_executable(lexis_app src/main.cpp)

# QML module: registers AppController/CorpusListModel/DocumentListModel
# (QML_ELEMENT-tagged) for `import Lexis` and compiles qml/*.qml into
# the binary (qt_add_qml_module handles the resource embedding -- no
# separate .qrc file needed). LexisEngine/IngestWorker/the extractors
# are plain C++ backend logic with no QML_ELEMENT of their own (nothing
# in QML ever names them directly), so they're listed as ordinary
# SOURCES here rather than needing their own registration.
# pragma Singleton inside Theme.qml only takes effect once the
# generated qmldir also marks the type "singleton" -- without this,
# qt_add_qml_module emits a plain "Theme 1.0 Theme.qml" qmldir entry,
# every Theme.* property access silently evaluates to undefined at
# runtime (confirmed: "Unable to assign [undefined] to QColor" for
# every single token, across every file), and yet the type itself still
# resolves fine (no "Theme unavailable" error), which is what let this
# go unnoticed until enough files referenced Theme to make the pattern
# obvious. Must be set before the QML_FILES entry below is processed.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(lexis_app
    URI Lexis
    VERSION 1.0
    QML_FILES
        qml/Theme.qml
        qml/Main.qml
        qml/GroupSidebar.qml
        qml/GroupsListView.qml
        qml/GroupDocumentsView.qml
        qml/ChatPanel.qml
    SOURCES
        src/AppController.h
        src/AppController.cpp
        src/CorpusListModel.h
        src/CorpusListModel.cpp
        src/DocumentListModel.h
        src/DocumentListModel.cpp
        src/ChatMessageListModel.h
        src/ChatMessageListModel.cpp
        src/ChatSessionListModel.h
        src/ChatSessionListModel.cpp
        src/LexisEngine.h
        src/LexisEngine.cpp
        src/IngestWorker.h
        src/IngestWorker.cpp
        src/ModelLoader.h
        src/ModelLoader.cpp
        src/QueryWorker.h
        src/QueryWorker.cpp
        src/DocxExtractor.h
        src/DocxExtractor.cpp
        src/PdfExtractor.h
        src/PdfExtractor.cpp
        src/OcrExtractor.h
        src/OcrExtractor.cpp
)
# qt_add_qml_module()'s generated type-registration file includes its
# QML_ELEMENT headers via angle brackets (<AppController.h>, guarded by
# __has_include) rather than relative to their own source location --
# needs src/ on the include path or those includes silently no-op and
# the generated file fails to compile with "undeclared identifier".
target_include_directories(lexis_app PRIVATE ${PG_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(lexis_app PRIVATE LEXIS_TESSDATA_PREFIX="${LEXIS_TESSDATA_PREFIX}")
target_link_libraries(lexis_app PRIVATE lexis_core Qt6::Quick Qt6::QuickControls2
    PkgConfig::POPPLER_CPP PkgConfig::PUGIXML PkgConfig::TESSERACT PkgConfig::LEPTONICA PkgConfig::LIBZIP)

# Batch evaluation harness. Links the app's own QueryWorker so a measurement
# run exercises the exact routing/retrieval/generation code the UI runs --
# every earlier measurement in this project used a scratch program that
# mirrored the pipeline by hand, and a mirror drifts. Qt6::Core only: no QML,
# no widgets, nothing graphical.
qt_add_executable(lexis_eval
    src/eval_main.cpp
    src/QueryWorker.h
    src/QueryWorker.cpp
)
target_include_directories(lexis_eval PRIVATE ${PG_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lexis_eval PRIVATE lexis_core Qt6::Core)
set_target_properties(lexis_eval PROPERTIES
    BUILD_RPATH "${LLAMA_CPP_DIR}/lib;${GGML_DIR}/lib"
)

# Needed to actually run the built binary straight out of the build
# directory during development (no install step) -- lexis_core's link
# requirements above only get the *linker* to llama.cpp/ggml at build
# time; the dynamic loader needs its own path at run time too, same
# reason the Makefile passes -Wl,-rpath for the CLI binary.
set_target_properties(lexis_app PROPERTIES
    BUILD_RPATH "${LLAMA_CPP_DIR}/lib;${GGML_DIR}/lib"
)
