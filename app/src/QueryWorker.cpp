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
#include "pg_store.h"
#include "query_formulation.h"
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
// Retrieval depth is chosen per query now, not fixed. The CLI's own
// LEXIS_TOP_K (main.c) is deliberately left at 5: it feeds the MS MARCO
// eval harness, where a changed retrieval depth would silently invalidate
// comparisons against previously recorded runs. The two are allowed to
// differ; see LIMITATIONS.md.
//
// kCandidateCeiling is how deep to ask BM25 to rank, NOT how much reaches
// the model -- bm25_result_set_trim() cuts that down.
constexpr size_t kCandidateCeiling = 40;

// The three limits, in the order they usually bind. Values come from
// measurement against a real corpus rather than taste:
//
//  - kMaxPassages 12: depth 5 and 10 both produced correct answers to a
//    "list every X" question, depth 20 fused a same-named label from an
//    unrelated section into the answer, and depth 30 collapsed it. The
//    usable window ends well before the context window does.
//  - kTokenBudget 1500: roughly where depth 12 lands at this chunk size,
//    and it leaves the bulk of LOCAL_LLM_N_CTX for conversation history,
//    which generation.c windows against whatever the prompt leaves free.
//  - kScoreFloorRatio 0.6: on the measured queries this cuts exactly the
//    flat noise tail (a query scoring 5.87 down to 2.6 gets cut after
//    rank 4) while leaving genuinely competitive runs intact (a query
//    scoring 8.80 down to 5.27 keeps ~10).
constexpr size_t kMaxPassages = 12;
constexpr int kTokenBudget = 1500;
constexpr double kScoreFloorRatio = 0.6;

// Persisted provenance for one answer: which tool ran, and what it
// retrieved. Stored as a JSON *object*, not the bare array this used to
// write, because the tool name is a property of the answer rather than of
// any one source -- and for the CHAT path there are no sources at all,
// yet "no tool was called" is exactly what the UI needs to say.
//
// chat_messages.sources is JSONB, so this needs no migration. Readers
// must still accept the legacy bare-array shape; see
// AppController::selectChatSession().
QString provenanceToJson(const QString &tool, const QVariantList &sources) {
    QJsonObject root;
    root[QStringLiteral("tool")] = tool;
    root[QStringLiteral("passages")] = QJsonArray::fromVariantList(sources);
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

// SEARCH path -- today's pipeline, extracted unchanged from before the
// tool router existed. Returns false (ok=false) only on a real failure;
// a "nothing to search for"/"no matching passages" outcome is a real,
// friendly answer string, not a failure -- see QueryWorker.h's own
// comment on why `answer` is never empty on success.
bool runSearchPipeline(PgStore *store, const char *questionCstr, const std::vector<LocalLlmTurn> &turns,
                        const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                        QString *answerOut, QVariantList *sourcesOut) {
    char *reformulated = query_formulation_contextualize_question(questionCstr, turns.data(), turns.size());
    if (reformulated == nullptr) {
        return false;
    }

    // The UNION of the raw question's terms and the reformulated one's,
    // not the reformulation alone. Reformulation resolves follow-ups like
    // "what about that instead?" into something BM25 can work with, which
    // is why it exists -- but it paraphrases, and a paraphrase can drop
    // the one term the index is keyed on. Measured: "What are the license
    // classes?" was rewritten to "What are the different types of driver
    // licenses in New York State?", losing "class"; the raw question found
    // the passages listing every class at ranks 1-2, the rewrite found
    // none of them. Taking the union lets the rewrite only ever add.
    TokenList *terms =
        query_formulation_terms_union(questionCstr, reformulated, stopwords, wordnet, lemmatizer);
    free(reformulated);
    if (terms == nullptr) {
        return false;
    }
    if (terms->count == 0) {
        token_list_free(terms);
        *answerOut = QObject::tr("I don't have enough to search for in that question -- could you rephrase it?");
        return true;
    }

    QVector<const char *> queryTerms;
    queryTerms.reserve(static_cast<int>(terms->count));
    for (size_t i = 0; i < terms->count; i++) {
        queryTerms.append(terms->terms[i]);
    }

    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25CorpusStats stats = bm25_corpus_stats(store);
    BM25ResultSet *results =
        (stats.total_passages >= 0)
            ? bm25_search(store, queryTerms.data(), terms->count, kCandidateCeiling, stats, params)
            : nullptr;
    token_list_free(terms);

    if (results == nullptr) {
        return false;
    }
    // Rank deep, send shallow: BM25 ranks up to kCandidateCeiling, then
    // this cuts to the prefix actually worth putting in front of the
    // model. Done here, once, before either the prompt or the provenance
    // payload is built from `results` -- that is what keeps "what the
    // model read" and "what the source inspector shows" the same list.
    bm25_result_set_trim(store, results, kMaxPassages, kTokenBudget, kScoreFloorRatio);
    if (results->count == 0) {
        bm25_result_set_free(results);
        *answerOut = QObject::tr("No matching passages found in this group for that question.");
        return true;
    }

    // Source citations, gathered the same way run_query() does --
    // fetched before generation so a passage that fails to load (rare,
    // but possible if the DB changed underneath a stale result set)
    // simply doesn't appear as a source, rather than aborting the query.
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
        // The passage text itself, not just its id. The UI's source
        // inspector shows the actual retrieved text -- a chunk id alone
        // tells the user nothing about what the answer was grounded in,
        // and re-reading the passage from the database at display time
        // would show whatever is there *now* rather than what this answer
        // was actually built from.
        source[QStringLiteral("text")] = QString::fromUtf8(passage->text);
        source[QStringLiteral("tokenCount")] = passage->token_count;
        sources.append(source);
        pg_store_passage_free(passage);
    }

    // The *original* question, not the reformulated search query -- the
    // reformulation only ever existed to help retrieval, not to replace
    // what the user actually asked (see generation.h's own doc comment).
    char *answer = generation_generate_answer_with_history(questionCstr, store, results, turns.data(), turns.size());
    bm25_result_set_free(results);
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

// Leaves room for the chat template's own markup plus the reply itself;
// the exact figure matters less than reserving *some* headroom, since
// LOCAL_LLM_N_CTX bounds prompt and generation together.
constexpr int kConverseReservedTokens = 2048;

bool runConversePipeline(const char *questionCstr, const std::vector<LocalLlmTurn> &turns, QString *answerOut) {
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

    char *answer = local_llm_chat_completion_multi(windowed.data(), windowed.size(), LEXIS_PREFILL_NO_THINK);
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
                         const std::vector<LocalLlmTurn> &turns, QString *answerOut, QVariantList *sourcesOut) {
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

    char *answer = generation_generate_answer_from_summary(questionCstr, summary, turns.data(), turns.size());
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
                          QObject *parent)
    : QThread(parent), m_conninfo(std::move(conninfo)), m_corpusId(corpusId), m_sessionId(sessionId),
      m_question(std::move(question)), m_stopwords(stopwords), m_wordnet(wordnet), m_lemmatizer(lemmatizer) {
}

void QueryWorker::run() {
    PgStore *store = pg_store_open(m_conninfo.toUtf8().constData());
    if (store == nullptr) {
        emit queryFinished(false, QString(), QVariantList(), QString());
        return;
    }
    if (pg_store_use_corpus(store, m_corpusId) != 0) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList(), QString());
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

    pg_store_append_chat_message(store, m_sessionId, 1, questionCstr, nullptr);

    ToolChoice tool =
        tool_router_choose_tool(questionCstr, turns.data(), turns.size(), previousAnswerUsedDocuments ? 1 : 0);

    QString answerText;
    QVariantList sources;
    bool ok = false;
    switch (tool) {
    case TOOL_SUMMARIZE_CORPUS:
        ok = runSummaryPipeline(store, m_corpusId, questionCstr, turns, &answerText, &sources);
        break;
    case TOOL_CONVERSE:
        // No store access and no `sources` -- the whole point of this
        // branch is that nothing was retrieved.
        ok = runConversePipeline(questionCstr, turns, &answerText);
        break;
    case TOOL_SEARCH_PASSAGES:
        ok = runSearchPipeline(store, questionCstr, turns, m_stopwords, m_wordnet, m_lemmatizer, &answerText, &sources);
        break;
    }

    pg_store_chat_messages_free(history_rows, history_count);

    if (!ok) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList(), QString());
        return;
    }

    // Always written now, even for CHAT with no sources: the tool name is
    // what the UI reports, so "nothing was retrieved" has to be recorded
    // rather than left as a NULL that reads identically to a legacy row.
    const QString tool_name = toolName(tool);
    const QString provenanceJson = provenanceToJson(tool_name, sources);
    pg_store_append_chat_message(store, m_sessionId, 0, answerText.toUtf8().constData(),
                                  provenanceJson.toUtf8().constData());
    pg_store_close(store);

    emit queryFinished(true, answerText, sources, tool_name);
}
