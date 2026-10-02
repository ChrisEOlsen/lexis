// Every question is routed into SEARCH (BM25 over reformulated query), SUMMARY (cached
// group overview), or CHAT (no retrieval). See QueryWorker.h for the pipeline order.
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
// Persisted provenance: which tool ran + what it retrieved. A JSON object (not the legacy
// bare array: CHAT has no sources yet "no tool was called" must still be recorded).
QString provenanceToJson(const QString &tool, const QVariantList &sources,
                          const QString &searchQuery, const QString &searchTerms) {
    QJsonObject root;
    root[QStringLiteral("tool")] = tool;
    root[QStringLiteral("passages")] = QJsonArray::fromVariantList(sources);
    // Written only when non-empty; missing keys reload as "" (the QML hide condition).
    if (!searchQuery.isEmpty()) {
        root[QStringLiteral("searchQuery")] = searchQuery;
    }
    if (!searchTerms.isEmpty()) {
        root[QStringLiteral("searchTerms")] = searchTerms;
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// Short lowercase tool tokens for storage; the UI owns user-facing wording.
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

// Refusal-shaped answer; triggers the one retry below. Keep in sync with
// scripts/eval_common.py REFUSAL_MARKERS (no shared code across C++/Python).
bool answerLooksLikeRefusal(const QString &answer) {
    static const char *markers[] = {
        "don't have enough", "do not have enough", "not enough information",
        "does not contain",  "doesn't contain",    "no matching passages",
        "could you rephrase", "cannot answer",     "can't answer",
        "please provide",    "i only have",        "i don't have access",
    };
    const QString lowered = answer.toLower();
    for (const char *marker : markers) {
        if (lowered.contains(QLatin1String(marker))) {
            return true;
        }
    }
    return false;
}

// Bridges the C streaming callback to this object's Qt signal (same thread, no lock needed).
// `pending` holds a multi-byte character split across two token pieces (else U+FFFD).
struct TokenBridge {
    QueryWorker *worker;
    QByteArray pending;
};

// Longest prefix of `buf` ending on a complete UTF-8 sequence (invalid input passes through).
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

// SUMMARY path forward declaration: the SEARCH fallback below can escalate to it,
// but its definition sits with the other pipelines further down.
bool runSummaryPipeline(PgStore *store, qint64 corpusId, const char *questionCstr,
                        const std::vector<LocalLlmTurn> &turns, QueryWorker *worker, int thinkingOverride,
                        QString *answerOut, QVariantList *sourcesOut, bool *modelFailedOut);

// True when the group holds at least one document. A DB error reads as empty; the
// summary path's "no documents" message is the graceful outcome either way.
bool groupHasDocuments(PgStore *store) {
    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    if (docs != nullptr) {
        pg_store_documents_free(docs, doc_count);
    }
    return doc_count > 0;
}

// READ path: answer from the documents' full text instead of keyword-matched passages.
// The first leg of the SEARCH fallback (small groups only). Same convention as the other
// pipelines: true means an answer string was produced; false with tooBigOut means the
// corpus exceeded the read budget (the summary leg is next); false with modelFailedOut
// means the model went quiet.
bool runReadPipeline(PgStore *store, const char *questionCstr, const std::vector<LocalLlmTurn> &turns,
                     QueryWorker *worker, int thinkingOverride, QString *answerOut, QVariantList *sourcesOut,
                     bool *tooBigOut, bool *modelFailedOut) {
    TokenBridge bridge{worker};

    emit worker->queryStage(StageReadingDocuments, -1);
    int tooBig = 0;
    char *answer = generation_generate_answer_from_documents_stream(
        questionCstr, store, turns.data(), turns.size(), thinkingOverride, token_trampoline, &bridge, &tooBig);
    *tooBigOut = (tooBig != 0);
    if (answer == nullptr) {
        if (!*tooBigOut && modelFailedOut != nullptr) {
            *modelFailedOut = true;
        }
        return false;
    }
    *answerOut = QString::fromUtf8(answer);
    free(answer);

    // What the answer was read from: every document's full text, mirroring how
    // collectSources keeps every passage's (a second fetch; the generate call freed its copy).
    QVariantList sources;
    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    if (docs != nullptr) {
        for (size_t i = 0; i < doc_count; i++) {
            if (docs[i].text == nullptr) {
                continue;
            }
            QVariantMap source;
            source[QStringLiteral("documentName")] = QString::fromUtf8(docs[i].document_name);
            source[QStringLiteral("text")] = QString::fromUtf8(docs[i].text);
            sources.append(source);
        }
        pg_store_documents_free(docs, doc_count);
    }
    *sourcesOut = sources;
    return true;
}

// SEARCH fallback for lexical recall failure: the question's words match nothing (or the
// wrong passages), so answer from the full text when the group fits, else from the cached
// overview. toolOut reports which leg produced the answer ("read"/"summary"). A fallback
// refusal is still adopted in normal mode -- the read consulted strictly more evidence
// than the failed search, so even its negative supersedes. fromRetry must not destroy:
// there a refusal-shaped fallback answer declines and the caller keeps the original.
bool runSearchFallback(PgStore *store, qint64 corpusId, const char *questionCstr,
                       const std::vector<LocalLlmTurn> &turns, QueryWorker *worker, int thinkingOverride,
                       bool fromRetry, QString *answerOut, QVariantList *sourcesOut, QString *toolOut,
                       bool *modelFailedOut) {
    if (!groupHasDocuments(store)) {
        // No read to attempt; the summary path owns the "no documents" message.
        if (!runSummaryPipeline(store, corpusId, questionCstr, turns, worker, thinkingOverride, answerOut,
                                sourcesOut, modelFailedOut)) {
            return false;
        }
        *toolOut = QStringLiteral("summary");
        return true;
    }

    bool tooBig = false;
    QString readAnswer;
    QVariantList readSources;
    if (runReadPipeline(store, questionCstr, turns, worker, thinkingOverride, &readAnswer, &readSources,
                        &tooBig, modelFailedOut)) {
        if (fromRetry && answerLooksLikeRefusal(readAnswer)) {
            return false;
        }
        *answerOut = readAnswer;
        *sourcesOut = readSources;
        *toolOut = QStringLiteral("read");
        return true;
    }
    if (!tooBig) {
        // The model went quiet; the summary leg would fail the same way.
        return false;
    }
    if (!runSummaryPipeline(store, corpusId, questionCstr, turns, worker, thinkingOverride, answerOut,
                            sourcesOut, modelFailedOut)) {
        return false;
    }
    if (fromRetry && answerLooksLikeRefusal(*answerOut)) {
        return false;
    }
    *toolOut = QStringLiteral("summary");
    return true;
}

// Source citations; a passage that fails to load is skipped, not fatal.
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
        // The passage text itself, so the inspector shows what the answer used.
        source[QStringLiteral("text")] = QString::fromUtf8(passage->text);
        source[QStringLiteral("tokenCount")] = passage->token_count;
        sources.append(source);
        pg_store_passage_free(passage);
    }
    return sources;
}

// SEARCH path. False only on real failure; empty-result outcomes are friendly answer strings.
// fromRetry runs the deeper policy from the start with no refusal pre-check.
// modelFailedOut (nullable) reports a dead model call, distinct from other failures.
// toolOut is set to "read"/"summary" when the fallback produced the answer, else untouched.
bool runSearchPipeline(PgStore *store, qint64 corpusId, const char *questionCstr,
                       const std::vector<LocalLlmTurn> &turns, const StopwordSet *stopwords,
                       const WordNetTable *wordnet, const Lemmatizer *lemmatizer, QueryWorker *worker,
                       bool fromRetry, int thinkingOverride, QString *answerOut, QVariantList *sourcesOut,
                       QString *searchQueryOut, QString *searchTermsOut, QString *toolOut,
                       bool *modelFailedOut) {
    TokenBridge bridge{worker};

    char *reformulated = query_formulation_contextualize_question(questionCstr, turns.data(), turns.size());
    if (reformulated == nullptr) {
        return false;
    }
    // Show the rewrite only when it changed something (standalone questions echo back).
    if (strcmp(reformulated, questionCstr) != 0) {
        *searchQueryOut = QString::fromUtf8(reformulated);
    }

    // Retrieval itself is one shared call (retrieval.c, same as CLI/eval); this function
    // owns only the chat-specific parts: contextualization, provenance, generation.
    emit worker->queryStage(fromRetry ? StageRetrying : StageSearching, -1);
    RetrievalPolicy policy = retrieval_default_policy();
    if (fromRetry) {
        // Retry parameters: score floor off (full passage budget), reasoning forced on.
        policy.score_floor_ratio = 0.0;
    }
    RetrievalRun *run =
        retrieval_run(store, questionCstr, reformulated, stopwords, wordnet, lemmatizer, &policy);
    if (run == nullptr) {
        free(reformulated);
        return false;
    }
    // Nothing lexical to work with: the fallback reads what the question's words can't
    // reach. The rewrite is cleared on adoption -- retrieval contributed nothing to a
    // fallback answer. On a retry, a declined fallback is a failure (caller keeps the
    // original) -- replacing a real answer with "no matches" would destroy it.
    if (run->terms->count == 0 || run->results->count == 0) {
        const bool noTerms = (run->terms->count == 0);
        free(reformulated);
        retrieval_run_free(run);
        QString fallbackAnswer;
        QVariantList fallbackSources;
        QString fallbackTool;
        if (runSearchFallback(store, corpusId, questionCstr, turns, worker, thinkingOverride, fromRetry,
                              &fallbackAnswer, &fallbackSources, &fallbackTool, modelFailedOut)) {
            *answerOut = fallbackAnswer;
            *sourcesOut = fallbackSources;
            *toolOut = fallbackTool;
            searchQueryOut->clear();
            return true;
        }
        if (fromRetry || (modelFailedOut != nullptr && *modelFailedOut)) {
            return false;
        }
        *answerOut = noTerms
                         ? QObject::tr("I don't have enough to search for in that question -- could you rephrase it?")
                         : QObject::tr("No matching passages found in this group for that question.");
        return true;
    }

    emit worker->queryStage(StageReading, static_cast<int>(run->results->count));
    emit worker->queryStage(StageWriting, -1);

    // Generates from the *original* question (the rewrite only helped retrieval).
    char *answer = generation_generate_answer_with_history_stream(
        questionCstr, store, run->results, turns.data(), turns.size(),
        fromRetry ? 1 : thinkingOverride, token_trampoline, &bridge);

    // One refusal retry: floor off (full budget) + reasoning on. Costs only the ~2% that refuse.
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

    // The search retrieved but the answer still refuses: the passages were the wrong ones
    // (a recall failure -- the retry only re-ranks the same lexical candidates), so fall
    // back to reading what the question's words couldn't reach. A declined fallback leaves
    // the search refusal standing, with search provenance below.
    if (answer != nullptr && answerLooksLikeRefusal(QString::fromUtf8(answer))) {
        QString fallbackAnswer;
        QVariantList fallbackSources;
        QString fallbackTool;
        if (runSearchFallback(store, corpusId, questionCstr, turns, worker, thinkingOverride, fromRetry,
                              &fallbackAnswer, &fallbackSources, &fallbackTool, modelFailedOut)) {
            free(answer);
            free(reformulated);
            retrieval_run_free(run);
            *answerOut = fallbackAnswer;
            *sourcesOut = fallbackSources;
            *toolOut = fallbackTool;
            searchQueryOut->clear();
            return true;
        }
        if (modelFailedOut != nullptr && *modelFailedOut) {
            free(answer);
            free(reformulated);
            retrieval_run_free(run);
            return false;
        }
    }
    free(reformulated);

    // Provenance from whichever run produced the final answer.
    char *joined_terms = ingest_join_words(run->terms, 0, run->terms->count);
    if (joined_terms != nullptr) {
        *searchTermsOut = QString::fromUtf8(joined_terms);
        free(joined_terms);
    }
    QVariantList sources = collectSources(store, run->results);
    retrieval_run_free(run);

    if (answer == nullptr) {
        if (modelFailedOut != nullptr) {
            *modelFailedOut = true;
        }
        return false;
    }
    *answerOut = QString::fromUtf8(answer);
    free(answer);
    *sourcesOut = sources;
    return true;
}

// CHAT path: no retrieval; the model answers from conversation alone (history windowed
// to fit the context window, oldest turns dropped first).

// Room for the chat template's markup, the reply, and the reasoning trace.
constexpr int kConverseReservedTokens = LOCAL_LLM_MAX_NEW_TOKENS + 256;

bool runConversePipeline(const char *questionCstr, const std::vector<LocalLlmTurn> &turns, QueryWorker *worker,
                         QString *answerOut, bool *modelFailedOut) {
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
    // The instruction block keeps the model aware a document collection is attached.
    QByteArray conversePrompt = QByteArray(LEXIS_PROMPT_CONVERSE_HEAD) + questionCstr;
    windowed.push_back(LocalLlmTurn{"user", conversePrompt.constData()});

    emit worker->queryStage(StageWriting, -1);

    // Reasoning always on here (not config-gated): CHAT messages are short, and history
    // lookups ("what was my question before that?") fail without it.
    char *answer = local_llm_chat_completion_multi_ex_stream(windowed.data(), windowed.size(), NULL, 1,
                                                              token_trampoline, &bridge);
    if (answer == nullptr) {
        if (modelFailedOut != nullptr) {
            *modelFailedOut = true;
        }
        return false;
    }
    *answerOut = QString::fromUtf8(answer);
    free(answer);
    return true;
}

// SUMMARY path: broad questions answered from the cached group overview (corpus_summary.h),
// built lazily here so every local_llm_* call stays on this serialized worker thread.
bool runSummaryPipeline(PgStore *store, qint64 corpusId, const char *questionCstr,
                         const std::vector<LocalLlmTurn> &turns, QueryWorker *worker, int thinkingOverride,
                         QString *answerOut, QVariantList *sourcesOut, bool *modelFailedOut) {
    TokenBridge bridge{worker};

    emit worker->queryStage(StageSummarizing, -1);
    char *summary = corpus_summary_get_or_build(store, static_cast<int64_t>(corpusId));
    if (summary == nullptr) {
        // A real answer, not a failure (same convention as empty search results).
        *answerOut = QObject::tr("There are no documents in this group yet, so there is nothing to summarize. "
                                  "Add documents and ask again.");
        return true;
    }

    emit worker->queryStage(StageWriting, -1);
    char *answer = generation_generate_answer_from_summary_stream(questionCstr, summary, turns.data(), turns.size(),
                                                                   thinkingOverride, token_trampoline, &bridge);
    if (answer == nullptr) {
        free(summary);
        if (modelFailedOut != nullptr) {
            *modelFailedOut = true;
        }
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

    // Load history before persisting the new question (else it sees itself as the last turn).
    // A fetch failure degrades to "no history" rather than aborting the query.
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
        // Drop the answer being retried from context (re-feeding it anchors to the failure).
        turns.pop_back();
        // And the question itself (its row already exists); else the model sees it twice.
        if (!turns.empty() && turns.back().role == std::string("user")) {
            turns.pop_back();
        }
    }

    // Did the previous answer use retrieval? The router needs this for elliptical
    // follow-ups ("is that all?" looks like filler alone). Read from stored provenance.
    bool previousAnswerUsedDocuments = false;
    for (size_t i = history_count; i-- > 0;) {
        if (history_rows[i].is_user) {
            continue;
        }
        const char *sources = history_rows[i].sources_json;
        if (sources != nullptr) {
            previousAnswerUsedDocuments =
                strstr(sources, "\"tool\":\"search\"") != nullptr ||
                strstr(sources, "\"tool\":\"summary\"") != nullptr || strstr(sources, "\"tool\":\"read\"") != nullptr;
        }
        break; // only the most recent answer matters
    }

    if (!m_forceRetry) {
        // A retry's question row already exists; only fresh questions get persisted.
        pg_store_append_chat_message(store, m_sessionId, 1, questionCstr, nullptr);
    }

    QString answerText;
    QVariantList sources;
    QString searchQuery;
    QString searchTerms;
    bool ok = false;
    bool modelFailed = false;
    QString tool_name;

    if (m_forceRetry) {
        // "Try harder": re-run SEARCH with the deeper policy, no re-routing.
        tool_name = QStringLiteral("search");
        ok = runSearchPipeline(store, m_corpusId, questionCstr, turns, m_stopwords, m_wordnet, m_lemmatizer,
                               this, /*fromRetry=*/true, m_thinkingOverride, &answerText, &sources,
                               &searchQuery, &searchTerms, &tool_name, &modelFailed);
    } else {
        emit queryStage(StageRouting, -1);
        int routerModelFailed = 0;
        ToolChoice tool = tool_router_choose_tool(questionCstr, turns.data(), turns.size(),
                                                  previousAnswerUsedDocuments ? 1 : 0, &routerModelFailed);
        if (routerModelFailed) {
            // The model never answered routing; running SEARCH anyway would only fail
            // again at generation with a misleading "no answer" message.
            modelFailed = true;
        } else {
            switch (tool) {
            case TOOL_SUMMARIZE_CORPUS:
                tool_name = toolName(tool);
                ok = runSummaryPipeline(store, m_corpusId, questionCstr, turns, this, m_thinkingOverride,
                                        &answerText, &sources, &modelFailed);
                break;
            case TOOL_CONVERSE:
                // No store access: nothing was retrieved on this path.
                tool_name = toolName(tool);
                ok = runConversePipeline(questionCstr, turns, this, &answerText, &modelFailed);
                break;
            case TOOL_SEARCH_PASSAGES:
                tool_name = toolName(tool);
                ok = runSearchPipeline(store, m_corpusId, questionCstr, turns, m_stopwords, m_wordnet,
                                       m_lemmatizer, this, /*fromRetry=*/false, m_thinkingOverride,
                                       &answerText, &sources, &searchQuery, &searchTerms, &tool_name,
                                       &modelFailed);
                break;
            }
        }
    }

    pg_store_chat_messages_free(history_rows, history_count);

    if (!ok) {
        pg_store_close(store);
        if (modelFailed) {
            emit queryFailed(
                tr("The local model didn't respond -- try again, or restart the app to free memory."));
        }
        emit queryFinished(false, QString(), QVariantList(), QString(), QString(), QString());
        return;
    }

    // Always written (even CHAT): "nothing was retrieved" must be recorded, not NULL.
    // A retry REPLACES the last assistant row so reloads show one answer, not two.
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