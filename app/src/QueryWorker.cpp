// Every question is first routed by tool_router_choose_tool() (a single
// LLM call, prefilled to skip thinking -- see local_llm_client.h's own
// doc comment) into one of two paths:
//
// SEARCH: today's existing pipeline, unchanged --
// query_formulation_contextualize_question() (a plain LLM call that
// resolves the question against conversation history into a standalone
// search query, e.g. "what about that instead?" -> "what is the minimum
// age for a Junior Operator Class MJ license?") -> BM25 search ->
// generation_generate_answer_with_history(). Good for specific,
// narrow questions.
//
// READ: skips reformulation and BM25 entirely -- answers directly from
// the full text of every document in the group instead of five scattered
// passages. Good for broad questions ("what is this document about?")
// BM25 retrieval structurally can't answer well.
//
// query_formulation_contextualize_question() is NOT the same thing as
// the WordNet-driven query_formulation_formulate_query() (still used by
// eval.c's --use-llm-expansion mode, not dead code) -- that one does
// synonym expansion, this one resolves follow-up references. See
// SPEED.md for why the WordNet expansion step was cut from the
// interactive chat pipeline in the first place (unrelated to why
// contextualization exists now).
#include "QueryWorker.h"

extern "C" {
#include "bm25.h"
#include "generation.h"
#include "ingest.h"
#include "pg_store.h"
#include "query_formulation.h"
#include "retrieval.h"
#include "corpus_summary.h"
#include "prompts.h"
#include "tool_router.h"
}

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
// Persisted provenance for one answer: which tool ran, and what it
// retrieved. Stored as a JSON *object*, not the bare array this used to
// write, because the tool name is a property of the answer rather than of
// any one source -- and for the CHAT path there are no sources at all,
// yet "no tool was called" is exactly what the UI needs to say.
//
// chat_messages.sources is JSONB, so this needs no migration. Readers
// must still accept the legacy bare-array shape; see
// AppController::selectChatSession().
QString provenanceToJson(const QString &tool, const QVariantList &sources,
                          const QString &searchQuery, const QString &searchTerms) {
    QJsonObject root;
    root[QStringLiteral("tool")] = tool;
    root[QStringLiteral("passages")] = QJsonArray::fromVariantList(sources);
    // Written only when non-empty: CHAT/SUMMARY rows stay in the exact
    // shape they had before these fields existed, and the reload path's
    // missing-key -> empty-string behavior is the QML hide condition.
    if (!searchQuery.isEmpty()) {
        root[QStringLiteral("searchQuery")] = searchQuery;
    }
    if (!searchTerms.isEmpty()) {
        root[QStringLiteral("searchTerms")] = searchTerms;
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// The wire/storage name for each tool. Kept as short lowercase tokens
// rather than the user-facing wording, so the UI owns presentation and
// stored rows don't need rewriting when that wording changes.
QString toolName(ToolChoice tool) {
    switch (tool) {
    case TOOL_SUMMARIZE_CORPUS:
        return QStringLiteral("summary");
    case TOOL_CONVERSE:
        return QStringLiteral("chat");
    case TOOL_SEARCH_PASSAGES:
        break;
    }
    return QStringLiteral("search");
}

// A refusal-shaped answer -- the model declining rather than answering.
// Mirrors scripts/*_score.py's REFUSAL_MARKERS. Used to trigger the one
// retry below; a false positive only costs one extra attempt.
bool answerLooksLikeRefusal(const QString &answer) {
    static const char *markers[] = {
        "don't have enough", "do not have enough", "not enough information",
        "does not contain",  "doesn't contain",    "no matching passages",
        "could you rephrase",
    };
    const QString lowered = answer.toLower();
    for (const char *marker : markers) {
        if (lowered.contains(QLatin1String(marker))) {
            return true;
        }
    }
    return false;
}

// Bridge between the C core's streaming callback (a plain function
// pointer + void*) and this object's Qt signal. One instance lives on
// the worker's stack for the duration of run(); the callback fires on
// that same thread, so emit is safe without locking.
//
// `pending` holds the bytes of a multi-byte character that the model
// split across two pieces: llama.cpp emits raw token bytes, and a token
// boundary falls mid-codepoint routinely for anything outside ASCII
// (accents, CJK, emoji). Decoding such a fragment on its own yields
// U+FFFD, so the tail waits here for the piece that completes it.
struct TokenBridge {
    QueryWorker *worker;
    QByteArray pending;
};

// Length of the longest prefix of `buf` that ends on a complete UTF-8
// sequence. Walks back over at most three continuation bytes to the
// lead byte and asks whether its sequence is all there; anything that
// isn't valid UTF-8 to begin with is passed through untouched for
// QString::fromUtf8() to handle exactly as before.
qsizetype completeUtf8Prefix(const QByteArray &buf) {
    const qsizetype size = buf.size();
    for (qsizetype back = 1; back <= 4 && back <= size; back++) {
        const auto byte = static_cast<unsigned char>(buf.at(size - back));
        if ((byte & 0xC0) == 0x80) {
            continue; // continuation byte: keep walking back
        }
        qsizetype needed = 1;
        if ((byte & 0xE0) == 0xC0) {
            needed = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            needed = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            needed = 4;
        }
        return back >= needed ? size : size - back;
    }
    return size;
}

void token_trampoline(const char *piece, size_t piece_len, void *user_data) {
    auto *bridge = static_cast<TokenBridge *>(user_data);
    bridge->pending.append(piece, static_cast<qsizetype>(piece_len));
    const qsizetype ready = completeUtf8Prefix(bridge->pending);
    if (ready == 0) {
        return; // the whole buffer is one unfinished character
    }
    const QByteArray complete = bridge->pending.left(ready);
    bridge->pending.remove(0, ready);
    emit bridge->worker->queryToken(QString::fromUtf8(complete));
}

// Source citations for the UI, fetched before generation so a passage
// that fails to load simply doesn't appear rather than aborting.
QVariantList collectSources(PgStore *store, const BM25ResultSet *results) {
    QVariantList sources;
    for (size_t i = 0; i < results->count; i++) {
        PgStorePassage *passage = pg_store_get_passage(store, results->items[i].passage_id);
        if (passage == nullptr) {
            continue;
        }
        QVariantMap source;
        source[QStringLiteral("documentName")] = QString::fromUtf8(passage->document_name);
        source[QStringLiteral("chunkId")] = passage->chunk_id;
        source[QStringLiteral("score")] = results->items[i].score;
        // The passage text itself, not just its id -- the source
        // inspector shows what this answer was actually built from, not
        // whatever the database holds at display time.
        source[QStringLiteral("text")] = QString::fromUtf8(passage->text);
        source[QStringLiteral("tokenCount")] = passage->token_count;
        sources.append(source);
        pg_store_passage_free(passage);
    }
    return sources;
}

// SEARCH path -- today's pipeline, extracted unchanged from before the
// tool router existed. Returns false (ok=false) only on a real failure;
// a "nothing to search for"/"no matching passages" outcome is a real,
// friendly answer string, not a failure -- see QueryWorker.h's own
// comment on why `answer` is never empty on success.
//
// fromRetry: called with forceRetry semantics -- the deeper retrieval
// policy from the start and no refusal pre-check (see QueryWorker.h).
// Otherwise the ordinary first pass runs, with the one automatic
// refusal retry after it.
bool runSearchPipeline(PgStore *store, const char *questionCstr, const std::vector<LocalLlmTurn> &turns,
                       const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                       QueryWorker *worker, bool fromRetry, int thinkingOverride, QString *answerOut,
                       QVariantList *sourcesOut, QString *searchQueryOut, QString *searchTermsOut) {
    TokenBridge bridge{worker};

    char *reformulated = query_formulation_contextualize_question(questionCstr, turns.data(), turns.size());
    if (reformulated == nullptr) {
        return false;
    }
    // Provenance for the source inspector. The rewrite is only worth
    // showing when it actually changed something -- for a standalone
    // question the model usually echoes it back verbatim.
    if (strcmp(reformulated, questionCstr) != 0) {
        *searchQueryOut = QString::fromUtf8(reformulated);
    }

    // The whole retrieval pipeline -- terms union, sense-filtered
    // expansion, weighted+coordinated BM25, trim -- is ONE shared call
    // (src/core/retrieval.c), the same one the CLI and eval run. This
    // function only owns what is chat-specific: contextualization above,
    // provenance capture, source citations, history-aware generation.
    emit worker->queryStage(fromRetry ? StageRetrying : StageSearching, -1);
    RetrievalPolicy policy = retrieval_default_policy();
    if (fromRetry) {
        // The measured refusal-retry parameters, on demand: score
        // floor off (fills the full passage budget with everything the
        // search found) -- and the reasoning pass is forced on for the
        // generation below, matching what the automatic retry does.
        policy.score_floor_ratio = 0.0;
    }
    RetrievalRun *run =
        retrieval_run(store, questionCstr, reformulated, stopwords, wordnet, lemmatizer, &policy);
    if (run == nullptr) {
        free(reformulated);
        return false;
    }
    // "Nothing to search for" and "no matching passages" are ordinary
    // answers on a first pass -- but on a retry they would REPLACE the
    // answer being improved (run() calls
    // pg_store_update_last_assistant_message() on success), destroying a
    // real answer because the second search happened to come up empty.
    // A retry that finds nothing is a failure, and the caller keeps the
    // original.
    if (run->terms->count == 0) {
        free(reformulated);
        retrieval_run_free(run);
        if (fromRetry) {
            return false;
        }
        *answerOut = QObject::tr("I don't have enough to search for in that question -- could you rephrase it?");
        return true;
    }
    if (run->results->count == 0) {
        free(reformulated);
        retrieval_run_free(run);
        if (fromRetry) {
            return false;
        }
        *answerOut = QObject::tr("No matching passages found in this group for that question.");
        return true;
    }

    emit worker->queryStage(StageReading, static_cast<int>(run->results->count));
    emit worker->queryStage(StageWriting, -1);

    // The *original* question, not the reformulated search query -- the
    // reformulation only ever existed to help retrieval, not to replace
    // what the user actually asked (see generation.h's own doc comment).
    // fromRetry always thinks (part of the measured retry parameters);
    // otherwise the caller's override decides (-1 = config).
    char *answer = generation_generate_answer_with_history_stream(
        questionCstr, store, run->results, turns.data(), turns.size(),
        fromRetry ? 1 : thinkingOverride, token_trampoline, &bridge);

    // Refusal retry, once. Measured on the 913-run: 8 of 19 refusals
    // happened with the gold passage already in context (the model
    // declined material it had), and several more with it just below the
    // score-floor cutoff. The retry attacks both at once: retrieve again
    // with the floor off (fills the full passage budget), regenerate
    // with the reasoning pass forced ON (the one case thinking measurably
    // rescued was exactly a model misreading passages it already had).
    // Cost lands only on the ~2% of queries that refuse.
    if (!fromRetry && answer != nullptr && answerLooksLikeRefusal(QString::fromUtf8(answer))) {
        emit worker->queryStage(StageRetrying, -1);
        RetrievalPolicy retryPolicy = retrieval_default_policy();
        retryPolicy.score_floor_ratio = 0.0;
        RetrievalRun *retryRun =
            retrieval_run(store, questionCstr, reformulated, stopwords, wordnet, lemmatizer, &retryPolicy);
        if (retryRun != nullptr && retryRun->results != nullptr && retryRun->results->count > 0) {
            emit worker->queryStage(StageReading, static_cast<int>(retryRun->results->count));
            emit worker->queryStage(StageWriting, -1);
            char *retryAnswer = generation_generate_answer_with_history_stream(
                questionCstr, store, retryRun->results, turns.data(), turns.size(), /*thinking=*/1,
                token_trampoline, &bridge);
            if (retryAnswer != nullptr && !answerLooksLikeRefusal(QString::fromUtf8(retryAnswer))) {
                free(answer);
                answer = retryAnswer;
                retrieval_run_free(run);
                run = retryRun;
                retryRun = nullptr;
            } else {
                free(retryAnswer);
            }
        }
        retrieval_run_free(retryRun); /* NULL-safe; no-op when adopted */
    }
    free(reformulated);

    // Provenance and citations from whichever run produced the final
    // answer -- what the model read and what the inspector shows must be
    // the same list.
    char *joined_terms = ingest_join_words(run->terms, 0, run->terms->count);
    if (joined_terms != nullptr) {
        *searchTermsOut = QString::fromUtf8(joined_terms);
        free(joined_terms);
    }
    QVariantList sources = collectSources(store, run->results);
    retrieval_run_free(run);

    if (answer == nullptr) {
        return false;
    }
    *answerOut = QString::fromUtf8(answer);
    free(answer);
    *sourcesOut = sources;
    return true;
}

// CHAT path -- no retrieval at all. The model answers the message
// directly, with the conversation as its only context.
//
// Windowed here rather than trusting the caller: history grows without
// bound across a long session, and local_llm_chat_completion_multi()
// fails outright (returns NULL) if the templated prompt doesn't fit in
// the context window. Dropping the oldest turns degrades a reply; a NULL
// would surface as "the query failed" for someone who typed "thanks".
//
// Reasoning-skip prefill comes from prompts.h (LEXIS_PREFILL_NO_THINK),
// shared with every other model call in the project.

// Leaves room for the chat template's own markup plus the reply itself.
// Derived from the generation cap rather than a literal because the two
// must move together (same lesson as generation.c's
// GENERATION_RESERVED_OUTPUT_TOKENS) -- and this path now runs the
// reasoning pass, whose trace alone has been measured past 512 tokens.
constexpr int kConverseReservedTokens = LOCAL_LLM_MAX_NEW_TOKENS + 256;

bool runConversePipeline(const char *questionCstr, const std::vector<LocalLlmTurn> &turns, QueryWorker *worker,
                         QString *answerOut) {
    TokenBridge bridge{worker};

    int budget = LOCAL_LLM_N_CTX - kConverseReservedTokens;
    int questionTokens = local_llm_count_tokens(questionCstr);
    if (questionTokens > 0) {
        budget -= questionTokens;
    }

    // Walk backwards, newest first, keeping whatever fits.
    size_t start = turns.size();
    int running = 0;
    for (size_t i = turns.size(); i-- > 0;) {
        int turnTokens = local_llm_count_tokens(turns[i].content);
        if (turnTokens < 0) {
            break;
        }
        if (running + turnTokens > budget) {
            break;
        }
        running += turnTokens;
        start = i;
    }

    std::vector<LocalLlmTurn> windowed;
    windowed.reserve(turns.size() - start + 1);
    for (size_t i = start; i < turns.size(); i++) {
        windowed.push_back(turns[i]);
    }
    // The question carries an instruction block now. Without one this path
    // sent the bare question and the model answered as a general-purpose
    // assistant with no idea a document collection was attached -- see
    // LEXIS_PROMPT_CONVERSE_HEAD in prompts.h for the observed failure.
    QByteArray conversePrompt = QByteArray(LEXIS_PROMPT_CONVERSE_HEAD) + questionCstr;
    windowed.push_back(LocalLlmTurn{"user", conversePrompt.constData()});

    emit worker->queryStage(StageWriting, -1);

    // Reasoning pass ON for this path, unconditionally -- unlike answer
    // generation it is not config-gated. CHAT messages are rare and
    // short (greetings, meta-questions), so the latency cost is small,
    // and the observed failure mode without it was real: "what was my
    // question before that?" needs a two-step history lookup, and with
    // reasoning off the model recited its instruction header instead.
    char *answer = local_llm_chat_completion_multi_ex_stream(windowed.data(), windowed.size(), NULL, 1,
                                                              token_trampoline, &bridge);
    if (answer == nullptr) {
        return false;
    }
    *answerOut = QString::fromUtf8(answer);
    free(answer);
    return true;
}

// SUMMARY path -- answers broad, whole-collection questions from the
// group's cached overview (corpus_summary.h) instead of from document
// text. Replaced a READ path that called
// generation_generate_answer_from_documents() on every such question,
// feeding whole documents through the context window each time; the
// summary is built once per group and is a few hundred tokens, so this
// path's cost no longer grows with the corpus.
//
// The summary is built here, lazily, on the first broad question about a
// group -- which is also what keeps every local_llm_* call on this one
// already-serialized worker thread rather than adding an unserialized
// third caller inside IngestWorker. See corpus_summary.h.
//
// `sources` reports the summary itself as the first entry, with its text,
// followed by one entry per document it covers. The summary IS what the
// model read, so showing it is what makes the source inspector honest for
// this path -- a list of document names alone would imply the documents
// were read directly, which is exactly what this path does not do.
bool runSummaryPipeline(PgStore *store, qint64 corpusId, const char *questionCstr,
                         const std::vector<LocalLlmTurn> &turns, QueryWorker *worker, int thinkingOverride,
                         QString *answerOut, QVariantList *sourcesOut) {
    TokenBridge bridge{worker};

    emit worker->queryStage(StageSummarizing, -1);
    char *summary = corpus_summary_get_or_build(store, static_cast<int64_t>(corpusId));
    if (summary == nullptr) {
        // No documents, or generation failed. A real answer, not a failure:
        // the same convention runSearchPipeline() uses for "nothing to
        // search for" -- see QueryWorker.h on why `answer` is never empty
        // on success.
        *answerOut = QObject::tr("There are no documents in this group yet, so there is nothing to summarize. "
                                  "Add documents and ask again.");
        return true;
    }

    emit worker->queryStage(StageWriting, -1);
    char *answer = generation_generate_answer_from_summary_stream(questionCstr, summary, turns.data(), turns.size(),
                                                                   thinkingOverride, token_trampoline, &bridge);
    if (answer == nullptr) {
        free(summary);
        return false;
    }
    *answerOut = QString::fromUtf8(answer);
    free(answer);

    QVariantList sources;
    QVariantMap summarySource;
    summarySource[QStringLiteral("documentName")] = QObject::tr("Group summary (generated)");
    summarySource[QStringLiteral("text")] = QString::fromUtf8(summary);
    sources.append(summarySource);
    free(summary);

    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    for (size_t i = 0; i < doc_count; i++) {
        QVariantMap source;
        source[QStringLiteral("documentName")] = QString::fromUtf8(docs[i].document_name);
        sources.append(source);
    }
    pg_store_documents_free(docs, doc_count);

    *sourcesOut = sources;
    return true;
}
} // namespace

QueryWorker::QueryWorker(QString conninfo, qint64 corpusId, qint64 sessionId, QString question,
                         const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                         bool forceRetry, int thinkingOverride, QObject *parent)
    : QThread(parent), m_conninfo(std::move(conninfo)), m_corpusId(corpusId), m_sessionId(sessionId),
      m_question(std::move(question)), m_stopwords(stopwords), m_wordnet(wordnet), m_lemmatizer(lemmatizer),
      m_forceRetry(forceRetry), m_thinkingOverride(thinkingOverride) {
}

void QueryWorker::run() {
    PgStore *store = pg_store_open(m_conninfo.toUtf8().constData());
    if (store == nullptr) {
        emit queryFinished(false, QString(), QVariantList(), QString(), QString(), QString());
        return;
    }
    if (pg_store_use_corpus(store, m_corpusId) != 0) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList(), QString(), QString(), QString());
        return;
    }

    QByteArray questionUtf8 = m_question.toUtf8();
    const char *questionCstr = questionUtf8.constData();

    // Load history BEFORE persisting the new question -- otherwise this
    // fetch would see its own not-yet-answered question as the most
    // recent "user" turn. A fetch failure degrades to "no history"
    // rather than aborting the query -- conversational context is an
    // enhancement here, not a precondition for answering at all.
    size_t history_count = 0;
    PgStoreChatMessage *history_rows = pg_store_get_chat_messages(store, m_sessionId, &history_count);
    if (history_rows == nullptr) {
        history_count = 0;
    }

    std::vector<LocalLlmTurn> turns(history_count);
    for (size_t i = 0; i < history_count; i++) {
        turns[i].role = history_rows[i].is_user ? "user" : "assistant";
        turns[i].content = history_rows[i].text;
    }

    if (m_forceRetry && !turns.empty() && turns.back().role == std::string("assistant")) {
        // A retry replaces the newest assistant answer -- that answer
        // must also not sit in the model's own context (it is the refusal
        // or near-miss being retried; re-feeding it anchors the
        // regeneration to the very text that failed). Dropping it gives
        // the retry exactly the conversation view the original answer
        // had: history, then the question.
        turns.pop_back();
        // And the question itself, which a retry did NOT append below
        // (its row already exists from the original ask). Leaving it
        // here would hand the model -- and
        // query_formulation_contextualize_question() -- the same
        // question twice, once as the last history turn and again as the
        // turn the pipeline appends.
        if (!turns.empty() && turns.back().role == std::string("user")) {
            turns.pop_back();
        }
    }

    // Did the previous answer in this conversation come from a retrieval
    // tool? The router needs this to read an elliptical follow-up correctly:
    // "is that all?" names no subject, so judged alone it looks like filler.
    // Scans backwards for the most recent assistant message and reads the
    // tool out of its stored provenance -- the same JSON the source
    // inspector reads, so no new column is needed.
    bool previousAnswerUsedDocuments = false;
    for (size_t i = history_count; i-- > 0;) {
        if (history_rows[i].is_user) {
            continue;
        }
        const char *sources = history_rows[i].sources_json;
        if (sources != nullptr) {
            previousAnswerUsedDocuments =
                strstr(sources, "\"tool\":\"search\"") != nullptr || strstr(sources, "\"tool\":\"summary\"") != nullptr;
        }
        break; // only the most recent answer matters
    }

    if (!m_forceRetry) {
        // A retry's question row already exists (it is the question this
        // worker is re-answering); only a fresh question gets persisted.
        pg_store_append_chat_message(store, m_sessionId, 1, questionCstr, nullptr);
    }

    QString answerText;
    QVariantList sources;
    QString searchQuery;
    QString searchTerms;
    bool ok = false;
    QString tool_name;

    if (m_forceRetry) {
        // "Try harder": the question was already routed to SEARCH and
        // its answer already exists; re-run retrieval with the deeper
        // policy. No re-routing (nothing changed about the question),
        // and the refusal pre-check is off -- the retry IS the request.
        tool_name = QStringLiteral("search");
        ok = runSearchPipeline(store, questionCstr, turns, m_stopwords, m_wordnet, m_lemmatizer, this,
                               /*fromRetry=*/true, m_thinkingOverride, &answerText, &sources, &searchQuery,
                               &searchTerms);
    } else {
        emit queryStage(StageRouting, -1);
        ToolChoice tool =
            tool_router_choose_tool(questionCstr, turns.data(), turns.size(), previousAnswerUsedDocuments ? 1 : 0);

        switch (tool) {
        case TOOL_SUMMARIZE_CORPUS:
            tool_name = toolName(tool);
            ok = runSummaryPipeline(store, m_corpusId, questionCstr, turns, this, m_thinkingOverride, &answerText,
                                    &sources);
            break;
        case TOOL_CONVERSE:
            // No store access and no `sources` -- the whole point of this
            // branch is that nothing was retrieved.
            tool_name = toolName(tool);
            ok = runConversePipeline(questionCstr, turns, this, &answerText);
            break;
        case TOOL_SEARCH_PASSAGES:
            tool_name = toolName(tool);
            ok = runSearchPipeline(store, questionCstr, turns, m_stopwords, m_wordnet, m_lemmatizer, this,
                                   /*fromRetry=*/false, m_thinkingOverride, &answerText, &sources, &searchQuery,
                                   &searchTerms);
            break;
        }
    }

    pg_store_chat_messages_free(history_rows, history_count);

    if (!ok) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList(), QString(), QString(), QString());
        return;
    }

    // Always written, even for CHAT with no sources: the tool name is
    // what the UI reports, so "nothing was retrieved" has to be recorded
    // rather than left as a NULL that reads identically to a legacy row.
    // A retry REPLACES the last assistant row instead of appending -- the
    // live UI shows one improved answer, and a session reloaded from
    // history must show the same single answer, not the refusal it fixed.
    const QString provenanceJson = provenanceToJson(tool_name, sources, searchQuery, searchTerms);
    if (m_forceRetry) {
        if (pg_store_update_last_assistant_message(store, m_sessionId, answerText.toUtf8().constData(),
                                                    provenanceJson.toUtf8().constData()) != 0) {
            pg_store_close(store);
            emit queryFinished(false, QString(), QVariantList(), QString(), QString(), QString());
            return;
        }
    } else {
        pg_store_append_chat_message(store, m_sessionId, 0, answerText.toUtf8().constData(),
                                      provenanceJson.toUtf8().constData());
    }
    pg_store_close(store);

    emit queryFinished(true, answerText, sources, tool_name, searchQuery, searchTerms);
}