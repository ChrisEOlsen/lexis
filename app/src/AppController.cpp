#include "AppController.h"
#include "ConfigManager.h"
#include "CorpusListModel.h"
#include "DocumentListModel.h"
#include "IngestWorker.h"
#include "LexisEngine.h"
#include "ModelLoader.h"
#include "QueryWorker.h"

extern "C" {
#include "config.h"
#include "local_llm_client.h"
#include "paths.h"
#include "retrieval.h"
}

#include <cstdlib>

#include <QClipboard>
#include <QDirIterator>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QModelIndex>
#include <QUrl>

namespace {
// Every path goes through the C core's paths module: relative to the
// working directory in a dev build (the historical behavior), absolute
// into the .app bundle's Resources / Application Support once main.cpp
// has called lexis_paths_set(). The conninfo and model path come from
// the config file either way.
const char *kStopwordsRelPath = "data/stopwords/english.txt";
const char *kWordnetRelDir = "data/wordnet";
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent), m_engine(nullptr), m_corpusModel(new CorpusListModel(this)),
      m_documentModel(new DocumentListModel(this)), m_chatModel(new ChatMessageListModel(this)),
      m_chatSessionModel(new ChatSessionListModel(this)), m_activeWorker(nullptr), m_modelLoader(nullptr),
      m_activeQueryWorker(nullptr), m_activeCorpusId(-1), m_activeChatSessionId(-1),
      m_activeChatSessionTitle(tr("New Chat")), m_busy(false), m_statusText(tr("Select a group")),
      m_modelReady(false), m_chatBusy(false), m_stopwords(nullptr), m_wordnet(nullptr), m_lemmatizer(nullptr) {
    char *conninfo = config_load_db_conninfo(lexis_paths_config_file());
    if (conninfo != nullptr) {
        m_connInfo = QString::fromUtf8(conninfo);
        free(conninfo);
    }
    m_engine = std::make_unique<LexisEngine>(m_connInfo);
    if (m_connInfo.isEmpty()) {
        emit notify(tr("No database configured. Set db_conninfo in config/lexis.conf "
                       "(copy config/lexis.conf.example and fill in your password)."));
    } else if (!m_engine->isConnected()) {
        emit notify(tr("Could not connect to the database. Is Postgres running (make pg-start)?"));
    } else {
        refreshCorpusModel();
    }

    char *stopwordsPath = lexis_paths_resource(kStopwordsRelPath);
    char *wordnetDir = lexis_paths_resource(kWordnetRelDir);
    m_stopwords = stopwordsPath != nullptr ? stopword_set_load(stopwordsPath) : nullptr;
    m_wordnet = wordnetDir != nullptr ? wordnet_table_load(wordnetDir) : nullptr;
    m_lemmatizer = wordnetDir != nullptr ? lemmatizer_load(wordnetDir) : nullptr;
    free(stopwordsPath);
    free(wordnetDir);
    if (m_stopwords == nullptr || m_wordnet == nullptr || m_lemmatizer == nullptr) {
        emit notify(tr("Could not load language data from data/stopwords or data/wordnet."));
    }

    // Kicked off immediately, not deferred to first chat use -- see
    // ModelLoader.h's own comment on why (~9-19s load time overlapping
    // with whatever the user does first, instead of stalling their
    // first question). Skipped quietly when the model file isn't there
    // yet -- that is the fresh-install state the setup overlay handles;
    // it calls retryModelLoad() when the download lands.
    char *modelPath = config_load_model_path(lexis_paths_config_file());
    m_modelPath = QString::fromUtf8(modelPath);
    free(modelPath);
    if (QFileInfo::exists(m_modelPath)) {
        retryModelLoad();
    }

    // Settings state (F5): read through the same file the Settings
    // panel will rewrite. The reranker's live gate starts in whatever
    // state the config implies -- the first query's lazy init reads the
    // config line itself, so an absent line needs no explicit call.
    const QString configPath = QString::fromUtf8(lexis_paths_config_file());
    m_thinkingEnabled = ConfigManager(configPath).thinkingEnabled();
    m_rerankerEnabled = ConfigManager(configPath).rerankerEnabled();
    m_modelDisplayName = QFileInfo(m_modelPath).fileName();
}

void AppController::retryModelLoad() {
    if (m_modelReady || m_modelLoader != nullptr) {
        return;
    }
    m_modelLoader = new ModelLoader(m_modelPath, this);
    connect(m_modelLoader, &ModelLoader::modelLoadFinished, this, &AppController::onModelLoadFinished);
    connect(m_modelLoader, &QThread::finished, m_modelLoader, &QObject::deleteLater);
    m_modelLoader->start();
}

AppController::~AppController() {
    // Real operations already in progress on other threads -- waiting
    // for them here (blocking app close briefly) is the correct, safe
    // behavior; destroying the language data below, or calling
    // local_llm_client_cleanup() while a query is still using the
    // model, out from under a still-running worker would not be.
    if (m_activeWorker != nullptr) {
        // Quitting mid-ingest: without the cancel this wait() blocked
        // the UI thread for the rest of a possibly minutes-long ingest
        // (observed as a beachball needing a force quit). Cancelling
        // first makes the wait end at the pipeline's next checkpoint --
        // moments, not minutes -- and is lossless (see cancelIngest()).
        m_activeWorker->requestCancel();
        m_activeWorker->wait();
    }
    if (m_modelLoader != nullptr) {
        m_modelLoader->wait();
    }
    if (m_activeQueryWorker != nullptr) {
        m_activeQueryWorker->wait();
    }
    // Safe to call even if init failed or was never reached (documented
    // in local_llm_client.h) -- no need to track success state here.
    local_llm_client_cleanup();

    stopword_set_free(m_stopwords);
    wordnet_table_free(m_wordnet);
    lemmatizer_free(m_lemmatizer);
}

bool AppController::isConnected() const {
    return m_engine && m_engine->isConnected();
}

qint64 AppController::activeCorpusId() const {
    return m_activeCorpusId;
}

QString AppController::activeCorpusName() const {
    return m_activeCorpusName;
}

bool AppController::isBusy() const {
    return m_busy;
}

QString AppController::statusText() const {
    return m_statusText;
}

CorpusListModel *AppController::corpusModel() const {
    return m_corpusModel;
}

DocumentListModel *AppController::documentModel() const {
    return m_documentModel;
}

ChatMessageListModel *AppController::chatModel() const {
    return m_chatModel;
}

ChatSessionListModel *AppController::chatSessionModel() const {
    return m_chatSessionModel;
}

qint64 AppController::activeChatSessionId() const {
    return m_activeChatSessionId;
}

QString AppController::activeChatSessionTitle() const {
    return m_activeChatSessionTitle;
}

bool AppController::isModelReady() const {
    return m_modelReady;
}

bool AppController::isChatBusy() const {
    return m_chatBusy;
}

int AppController::contextTokenLimit() const {
    return LOCAL_LLM_N_CTX;
}

bool AppController::createGroup(const QString &displayName) {
    qint64 id = 0;
    if (!m_engine->createCorpus(displayName, &id)) {
        emit notify(tr("Could not create group: %1").arg(m_engine->lastError()));
        return false;
    }
    refreshCorpusModel();
    return true;
}

bool AppController::deleteGroup(qint64 corpusId) {
    if (corpusId == m_ingestingCorpusId) {
        // Deleting a group whose index is mid-rebuild would race the
        // rebuild's final schema swap. Cancel first, delete after.
        emit notify(tr("This group is still ingesting. Cancel the ingestion first, then delete it."));
        return false;
    }
    if (!m_engine->deleteCorpus(corpusId)) {
        emit notify(tr("Could not delete group: %1").arg(m_engine->lastError()));
        return false;
    }
    if (corpusId == m_activeCorpusId) {
        m_activeCorpusId = -1;
        m_activeCorpusName.clear();
        m_documentModel->setDocuments({});
        // The chat state has to be torn down too, not just the documents.
        // Deleting the active group used to leave the conversation sitting
        // on screen and the deleted group's sessions still listed in the
        // history drawer -- both referring to rows the ON DELETE CASCADE
        // had already removed (public.chat_sessions.corpus_id references
        // public.corpora ON DELETE CASCADE, so the sessions and every
        // message in them go with the registry row). Clearing the session
        // model is separate from startNewChat(), which only resets the
        // active session and the message list.
        m_chatSessionModel->setSessions({});
        startNewChat();
        emit activeCorpusIdChanged();
        emit canRetryLastAnswerChanged();
    }
    refreshCorpusModel();
    return true;
}

void AppController::selectGroup(qint64 corpusId) {
    if (!m_engine->useCorpus(corpusId)) {
        emit notify(tr("Could not switch groups: %1").arg(m_engine->lastError()));
        return;
    }
    m_activeCorpusId = corpusId;

    QVector<Corpus> corpora;
    m_engine->listCorpora(&corpora);
    m_activeCorpusName = QString::number(corpusId);
    for (const Corpus &corpus : corpora) {
        if (corpus.id == corpusId) {
            m_activeCorpusName = corpus.displayName;
            break;
        }
    }

    refreshDocumentModel();

    // Opening a group always lands on a fresh chat, never on the tail of
    // whatever conversation happened last. Resuming is an explicit act
    // via the history drawer.
    //
    // This deliberately replaces the earlier "pick up where the user left
    // off" behavior, which auto-selected the most recent session. That
    // made the group's newest conversation load itself on every open,
    // which is wrong in two ways: a new question typed straight after
    // opening a group would silently append to an old conversation (and
    // be sent with its history as context, see QueryWorker), and there
    // was no way to reach the empty state at all without pressing
    // "New chat" every single time.
    //
    // The session list is still loaded here -- the history drawer reads
    // it, and it must reflect this group rather than the previous one.
    QVector<ChatSession> sessions;
    m_engine->listChatSessions(corpusId, &sessions);
    m_chatSessionModel->setSessions(sessions);
    startNewChat();

    emit activeCorpusIdChanged();
    emit canRetryLastAnswerChanged();
}

void AppController::startNewChat() {
    m_activeChatSessionId = -1;
    m_activeChatSessionTitle = tr("New Chat");
    m_chatModel->setMessages({});
    clearLastExchange();
    emit activeChatSessionIdChanged();
    emit canRetryLastAnswerChanged();
}

// The m_last* fields describe the newest exchange of the session on
// screen. They must be cleared or re-derived on every session change:
// left stale, retryLastAnswer()'s guards all pass and the retry runs the
// PREVIOUS session's question against the current session's id, then
// replaces this session's newest answer with the result.
void AppController::clearLastExchange() {
    m_lastQuestion.clear();
    m_lastAnswerWasSearch = false;
    m_lastAnswer.clear();
    m_lastAnswerSources.clear();
    m_lastAnswerTool.clear();
    m_lastAnswerSearchQuery.clear();
    m_lastAnswerSearchTerms.clear();
}

// Reads the newest exchange back out of a session loaded from history,
// so "Try harder" works there exactly as it does on a fresh answer. Only
// a trailing assistant message qualifies: the retry replaces the last
// answer, so there has to be one, and the question it re-asks is the
// user message immediately before it.
void AppController::adoptLastExchange(const QVector<ChatMessage> &messages) {
    clearLastExchange();
    if (messages.isEmpty() || messages.last().isUser) {
        return;
    }
    const ChatMessage &answer = messages.last();
    int questionIndex = -1;
    for (int i = messages.size() - 2; i >= 0; i--) {
        if (messages.at(i).isUser) {
            questionIndex = i;
            break;
        }
    }
    if (questionIndex < 0) {
        return; // an answer with no question above it: nothing to re-ask
    }
    m_lastQuestion = messages.at(questionIndex).text;
    m_lastAnswer = answer.text;
    m_lastAnswerSources = answer.sources;
    m_lastAnswerTool = answer.tool;
    m_lastAnswerSearchQuery = answer.searchQuery;
    m_lastAnswerSearchTerms = answer.searchTerms;
    m_lastAnswerWasSearch = (answer.tool == QStringLiteral("search"));
}

void AppController::selectChatSession(qint64 sessionId) {
    m_activeChatSessionId = sessionId;
    m_activeChatSessionTitle = m_chatSessionModel->titleForId(sessionId);

    QVector<ChatHistoryEntry> history;
    m_engine->getChatMessages(sessionId, &history);

    QVector<ChatMessage> messages;
    messages.reserve(history.size());
    for (const ChatHistoryEntry &entry : history) {
        QVariantList sources;
        QString tool;
        QString searchQuery;
        QString searchTerms;
        if (!entry.sourcesJson.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(entry.sourcesJson.toUtf8());
            // Two accepted shapes, deliberately. The current one is an
            // object, {"tool": ..., "passages": [...]}, written by
            // QueryWorker::provenanceToJson(). The bare array is the legacy
            // shape from before the tool was recorded; rows in that form
            // predate the CHAT path, and every one of them came from the
            // search pipeline, so naming the tool "search" for them is a
            // statement of fact rather than a guess. Reading them as an
            // object instead would silently drop their citations.
            if (doc.isObject()) {
                const QJsonObject root = doc.object();
                tool = root.value(QStringLiteral("tool")).toString();
                sources = root.value(QStringLiteral("passages")).toArray().toVariantList();
                // Absent on CHAT/SUMMARY rows and anything stored before
                // these fields existed -- .toString() yields empty, which
                // is the QML hide condition.
                searchQuery = root.value(QStringLiteral("searchQuery")).toString();
                searchTerms = root.value(QStringLiteral("searchTerms")).toString();
            } else if (doc.isArray()) {
                sources = doc.array().toVariantList();
                tool = QStringLiteral("search");
            }
        }
        messages.append(ChatMessage{entry.text, entry.isUser, sources, tool, searchQuery, searchTerms,
                                    /*isFresh=*/false});
    }
    m_chatModel->setMessages(messages);
    adoptLastExchange(messages);

    emit activeChatSessionIdChanged();
    emit canRetryLastAnswerChanged();
}

void AppController::deleteChatSession(qint64 sessionId) {
    if (!m_engine->deleteChatSession(sessionId)) {
        emit notify(tr("Could not delete chat: %1").arg(m_engine->lastError()));
        return;
    }
    if (sessionId == m_activeChatSessionId) {
        startNewChat();
    }
    refreshChatSessionModel();
}

void AppController::ingestFiles(const QStringList &fileUrls) {
    if (m_activeCorpusId < 0 || m_activeWorker != nullptr || fileUrls.isEmpty()) {
        // No group selected, a previous ingest is still running
        // (GroupContentView.qml disables drops while busy, so this
        // shouldn't normally be reachable -- guarding anyway rather
        // than starting two concurrent rebuilds of the same corpus), or
        // nothing was actually dropped.
        return;
    }

    QStringList localPaths;
    for (const QString &fileUrl : fileUrls) {
        QUrl url(fileUrl);
        QString localPath = url.isLocalFile() ? url.toLocalFile() : fileUrl;
        if (localPath.isEmpty()) {
            continue;
        }
        if (!QFileInfo(localPath).isDir()) {
            // A directly-dropped file goes through as-is, whatever its
            // type -- IngestWorker reports unsupported ones back as
            // "skipped", which is the right feedback for a deliberate
            // single-file drop.
            localPaths.append(localPath);
            continue;
        }
        // A dropped folder: walk it recursively and keep only the
        // types IngestWorker can extract (keep this list in sync with
        // its suffix dispatch). Everything else is ignored SILENTLY --
        // a real folder is full of incidental files (.DS_Store,
        // installers, whatever), and listing them all as "skipped"
        // would bury the useful part of the completion message.
        static const QStringList kSupportedSuffixes = {
            QStringLiteral("txt"),  QStringLiteral("csv"), QStringLiteral("docx"),
            QStringLiteral("pdf"),  QStringLiteral("png"), QStringLiteral("jpg"),
            QStringLiteral("jpeg"), QStringLiteral("tiff"), QStringLiteral("tif"),
            QStringLiteral("bmp")};
        QDirIterator it(localPath, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString filePath = it.next();
            if (kSupportedSuffixes.contains(QFileInfo(filePath).suffix().toLower())) {
                localPaths.append(filePath);
            }
        }
    }
    if (localPaths.isEmpty()) {
        // Reachable by dropping a folder with nothing usable inside --
        // silence here would read as the drop not registering at all.
        emit notify(tr("No supported documents found in the dropped folder."));
        return;
    }

    m_busy = true;
    emit busyChanged();
    m_statusText = tr("Processing %1 file(s)...").arg(localPaths.size());
    emit statusTextChanged();

    // Chat with THIS group is blocked until the ingest lands (the group
    // is mid-rebuild; answers would come from a half-built index --
    // observed: "there are no documents" while 33K documents were being
    // ingested). Every other group stays fully usable meanwhile.
    m_ingestingCorpusId = m_activeCorpusId;
    m_ingestProgress = 0.0;
    m_ingestAnimMs = 0;
    m_ingestStatusText = tr("Preparing...");
    emit ingestStateChanged();
    emit canRetryLastAnswerChanged();

    m_activeWorker = new IngestWorker(m_connInfo, m_activeCorpusId, localPaths, m_stopwords,
                                       m_wordnet, m_lemmatizer, this);
    connect(m_activeWorker, &IngestWorker::ingestFinished, this, &AppController::onIngestFinished);
    connect(m_activeWorker, &IngestWorker::ingestProgress, this, &AppController::onIngestProgress);
    connect(m_activeWorker, &QThread::finished, m_activeWorker, &QObject::deleteLater);
    m_activeWorker->start();
}

void AppController::cancelIngest() {
    if (m_activeWorker == nullptr) {
        return;
    }
    m_activeWorker->requestCancel();
    m_ingestStatusText = tr("Cancelling...");
    emit ingestStateChanged();
    emit canRetryLastAnswerChanged();
}

void AppController::onIngestProgress(int filesDone, int filesTotal, qint64 indexEtaMs) {
    if (indexEtaMs < 0) {
        // Extraction phase: real per-file progress, scaled into the
        // first 60% of the bar (the index rebuild owns the rest).
        m_ingestProgress = filesTotal > 0 ? 0.6 * double(filesDone) / double(filesTotal) : 0.0;
        m_ingestAnimMs = 250;
        m_ingestStatusText = tr("Reading documents (%1 of %2)...").arg(filesDone).arg(filesTotal);
    } else {
        // Index rebuild: one C call with no progress hooks. The bar's
        // target jumps to near-done and QML animates there over the
        // estimated duration -- honest movement, estimated pace.
        m_ingestProgress = 0.97;
        m_ingestAnimMs = int(qMin<qint64>(indexEtaMs, 30 * 60 * 1000));
        const qint64 seconds = indexEtaMs / 1000;
        m_ingestStatusText =
            seconds < 90 ? tr("Building the search index (about %1 seconds)...").arg(qMax<qint64>(seconds, 5))
                         : tr("Building the search index (about %1 minutes)...").arg((seconds + 30) / 60);
    }
    emit ingestStateChanged();
    emit canRetryLastAnswerChanged();
}

void AppController::onIngestFinished(bool ok, bool cancelled, qint64 totalPassages, QStringList skipped,
                                      QStringList malformed, QStringList noTextFound) {
    m_activeWorker = nullptr; // the object itself is cleaned up by the QThread::finished->deleteLater() connection
    m_busy = false;
    m_statusText = tr("Drag files here to add them to this group.");
    emit busyChanged();
    emit statusTextChanged();

    m_ingestingCorpusId = -1;
    m_ingestProgress = 0.0;
    m_ingestAnimMs = 0;
    m_ingestStatusText.clear();
    emit ingestStateChanged();
    emit canRetryLastAnswerChanged();

    if (ok && totalPassages > 0) {
        refreshDocumentModel();
    }

    if (cancelled) {
        // Nothing landed: the rebuild's temporary schema was dropped and
        // the group is exactly as it was before the drop.
        emit notify(tr("Ingestion cancelled. The group was left unchanged."));
        return;
    }

    QStringList messageParts;
    if (!ok) {
        messageParts.append(tr("Ingestion failed -- see the console for details."));
    } else if (totalPassages > 0) {
        messageParts.append(tr("Group now has %1 total passages.").arg(totalPassages));
    }
    if (!skipped.isEmpty()) {
        messageParts.append(tr("Not supported yet: %1").arg(skipped.join(QStringLiteral(", "))));
    }
    if (!malformed.isEmpty()) {
        messageParts.append(tr("Could not be read, not ingested: %1").arg(malformed.join(QStringLiteral(", "))));
    }
    if (!noTextFound.isEmpty()) {
        messageParts.append(tr("No text found -- an image with no recognizable text, or a scanned PDF "
                                "(not yet supported): %1")
                                 .arg(noTextFound.join(QStringLiteral(", "))));
    }
    if (!messageParts.isEmpty()) {
        emit notify(messageParts.join(QStringLiteral("\n\n")));
    }
}

void AppController::sendChatMessage(const QString &question) {
    if (m_activeCorpusId < 0 || !m_modelReady || m_activeQueryWorker != nullptr || question.trimmed().isEmpty()) {
        // No group selected, the model hasn't finished loading yet, a
        // previous query is still running (only one at a time --
        // local_llm_chat_completion() has no concurrency support of its
        // own, see QueryWorker.h), or an empty/whitespace-only message.
        return;
    }
    if (m_activeCorpusId == m_ingestingCorpusId) {
        // This group's index is mid-rebuild; an answer now would come
        // from a half-built (or momentarily empty) corpus. ChatPanel
        // already swaps the chat for a progress panel -- this guard is
        // the backstop in case a message slips through anyway.
        return;
    }

    if (m_activeChatSessionId < 0) {
        // First message of a new chat -- create its session row now,
        // titled from the question itself (truncated; no LLM call just
        // for titling, kept simple until proven insufficient). See
        // startNewChat()'s own comment on why this doesn't happen any
        // earlier than this.
        QString title = question.trimmed();
        constexpr int kMaxTitleLength = 60;
        if (title.length() > kMaxTitleLength) {
            title = title.left(kMaxTitleLength).trimmed() + QStringLiteral("...");
        }
        qint64 newSessionId = -1;
        if (!m_engine->createChatSession(m_activeCorpusId, title, &newSessionId)) {
            emit notify(tr("Could not start a new chat: %1").arg(m_engine->lastError()));
            return;
        }
        m_activeChatSessionId = newSessionId;
        m_activeChatSessionTitle = title;
        emit activeChatSessionIdChanged();
        refreshChatSessionModel();
    }

    m_chatModel->addMessage(question, true);

    m_chatBusy = true;
    emit chatBusyChanged();
    m_lastQuestion = question;
    m_lastAnswerWasSearch = false; // not known until the router picks a tool
    m_retryingLiveAnswer = false;
    emit canRetryLastAnswerChanged();
    m_queryStageText = tr("Working...");
    emit queryStageTextChanged();

    m_activeQueryWorker = new QueryWorker(m_connInfo, m_activeCorpusId, m_activeChatSessionId,
                                           question, m_stopwords, m_wordnet, m_lemmatizer, /*forceRetry=*/false,
                                           m_thinkingEnabled ? 1 : 0);
    connect(m_activeQueryWorker, &QueryWorker::queryFinished, this, &AppController::onQueryFinished);
    connect(m_activeQueryWorker, &QueryWorker::queryStage, this, &AppController::onQueryStage);
    connect(m_activeQueryWorker, &QueryWorker::queryToken, this, &AppController::onQueryToken);
    connect(m_activeQueryWorker, &QThread::finished, m_activeQueryWorker, &QObject::deleteLater);
    m_activeQueryWorker->start();
}

void AppController::onModelLoadFinished(bool ok) {
    m_modelLoader = nullptr; // cleaned up by the QThread::finished->deleteLater() connection
    m_modelReady = ok;
    emit modelReadyChanged();
    emit canRetryLastAnswerChanged();
    if (!ok) {
        emit notify(tr("Could not load the local model from %1.").arg(m_modelPath));
    }
}

void AppController::onQueryFinished(bool ok, QString answer, QVariantList sources, QString tool,
                                    QString searchQuery, QString searchTerms) {
    m_activeQueryWorker = nullptr; // cleaned up by the QThread::finished->deleteLater() connection
    m_chatBusy = false;
    emit chatBusyChanged();
    m_queryStageText.clear();
    emit queryStageTextChanged();

    if (!ok) {
        if (m_retryingLiveAnswer) {
            // A failed retry must not destroy the answer it was trying to
            // improve: put the original back (the persisted row was never
            // touched -- QueryWorker's replace runs only on success).
            m_chatModel->restoreLastAssistant(m_lastAnswer, m_lastAnswerSources, m_lastAnswerTool,
                                              m_lastAnswerSearchQuery, m_lastAnswerSearchTerms);
            m_retryingLiveAnswer = false;
            // m_lastAnswerWasSearch is deliberately NOT touched on this
            // path: the original answer is back, unchanged, so a second
            // "try harder" on the same exchange must still be offered.
            // (A failed query reports an empty tool, so assigning from it
            // here would silently disable the button.)
            emit canRetryLastAnswerChanged();
            emit notify(tr("Couldn't improve that answer -- the original is unchanged."));
            return;
        } else {
            // A failed query leaves no trace in the conversation (same as
            // before streaming: nothing was persisted) -- the live row, if
            // one ever appeared, goes away with it.
            m_chatModel->discardLive();
        }
        emit notify(tr("Could not answer that question -- see the console for details."));
        emit canRetryLastAnswerChanged();
        return;
    }
    m_retryingLiveAnswer = false;
    m_lastAnswerWasSearch = (tool == QStringLiteral("search"));
    m_lastAnswer = answer;
    m_lastAnswerSources = sources;
    m_lastAnswerTool = tool;
    m_lastAnswerSearchQuery = searchQuery;
    m_lastAnswerSearchTerms = searchTerms;
    // QueryWorker always sends a real, displayable answer on ok == true
    // now -- "nothing to search for" and "no matching passages" are
    // themselves the answer text (and already persisted to
    // chat_messages by QueryWorker), not a sentinel this slot has to
    // special-case into a synthesized message of its own; doing that
    // here would desync what's shown live from what's actually stored.
    // The live row (created on the first streamed token) is finalized
    // with the authoritative text; when no token ever arrived (empty
    // reply, or a stage that answers before generation) finishLive()
    // falls back to an ordinary append internally.
    m_chatModel->finishLive(answer, sources, tool, searchQuery, searchTerms);
    emit canRetryLastAnswerChanged();
}

void AppController::onQueryStage(int stage, int payload) {
    switch (stage) {
    case StageRouting:
        m_queryStageText = tr("Working...");
        break;
    case StageRewriting:
        m_queryStageText = tr("Refining the question...");
        break;
    case StageSearching:
        m_queryStageText = tr("Searching the group...");
        break;
    case StageReading:
        m_queryStageText = payload > 0 ? tr("Reading %1 passages...").arg(payload) : tr("Reading passages...");
        break;
    case StageWriting:
        m_queryStageText = tr("Writing...");
        break;
    case StageRetrying:
        m_queryStageText = tr("Trying again with a deeper search...");
        // The first attempt's refusal text was already streamed into the
        // live row; the retry regenerates from scratch, so clear it --
        // leaving the refusal on screen under the new answer would read
        // as two answers stacked in one message.
        m_chatModel->resetLiveText();
        break;
    case StageSummarizing:
        m_queryStageText = tr("Summarizing the group...");
        break;
    default:
        return;
    }
    emit queryStageTextChanged();
}

void AppController::onQueryToken(QString piece) {
    if (!m_chatModel->hasLiveAnswer()) {
        m_chatModel->beginLiveAnswer();
    }
    m_chatModel->appendLiveText(piece);
}

void AppController::copyToClipboard(const QString &text) {
    QGuiApplication::clipboard()->setText(text);
}

void AppController::setThinkingEnabled(bool enabled) {
    if (enabled == m_thinkingEnabled) {
        return;
    }
    ConfigManager manager(QString::fromUtf8(lexis_paths_config_file()));
    if (!manager.setThinkingEnabled(enabled)) {
        emit notify(tr("Could not save the setting: %1").arg(manager.lastError()));
        return;
    }
    m_thinkingEnabled = enabled;
    emit settingsChanged();
    // No live call needed: every QueryWorker takes m_thinkingEnabled as
    // its explicit per-query override (see sendChatMessage()).
}

void AppController::setRerankerEnabled(bool enabled) {
    if (enabled == m_rerankerEnabled) {
        return;
    }
    ConfigManager manager(QString::fromUtf8(lexis_paths_config_file()));
    if (!manager.setRerankerEnabled(enabled)) {
        emit notify(tr("Could not save the setting: %1").arg(manager.lastError()));
        return;
    }
    // The write already succeeded; apply the live gate. Off means the
    // model is never even loaded (see retrieval.c's reranker_user_enabled).
    retrieval_set_reranker_enabled(enabled ? 1 : 0);
    m_rerankerEnabled = enabled;
    emit settingsChanged();
}

QString AppController::configDirectoryUrl() const {
    const QString configPath = QString::fromUtf8(lexis_paths_config_file());
    const QString dir = QFileInfo(configPath).absolutePath();
    return QUrl::fromLocalFile(dir).toString();
}

// A query is already running, nothing was ever asked, the newest answer
// didn't come from SEARCH (a CHAT reply has no retrieval to deepen; a
// SUMMARY answer is about the overview, not the passages), or the group
// itself isn't in a state to answer.
bool AppController::canRetryLastAnswer() const {
    return m_activeQueryWorker == nullptr && !m_lastQuestion.isEmpty() && m_lastAnswerWasSearch &&
           m_activeCorpusId >= 0 && m_modelReady && m_activeChatSessionId >= 0 &&
           m_activeCorpusId != m_ingestingCorpusId;
}

void AppController::retryLastAnswer() {
    if (!canRetryLastAnswer()) {
        return;
    }

    // Convert the newest answer's row into the live row up front: the
    // retry replaces that answer in the UI (QueryWorker replaces it in
    // history too -- same single row before and after, whether tokens
    // streamed or not). m_lastAnswerWasSearch stays true, so a second
    // "try harder" click on the same exchange keeps working.
    m_chatModel->makeLastAssistantLive();
    if (!m_chatModel->hasLiveAnswer()) {
        // Nothing to replace (shouldn't happen given the guards above).
        return;
    }
    m_retryingLiveAnswer = true;

    m_chatBusy = true;
    emit chatBusyChanged();
    emit canRetryLastAnswerChanged();
    m_queryStageText = tr("Trying again with a deeper search...");
    emit queryStageTextChanged();

    m_activeQueryWorker = new QueryWorker(m_connInfo, m_activeCorpusId, m_activeChatSessionId, m_lastQuestion,
                                           m_stopwords, m_wordnet, m_lemmatizer, /*forceRetry=*/true,
                                           m_thinkingEnabled ? 1 : 0);
    connect(m_activeQueryWorker, &QueryWorker::queryFinished, this, &AppController::onQueryFinished);
    connect(m_activeQueryWorker, &QueryWorker::queryStage, this, &AppController::onQueryStage);
    connect(m_activeQueryWorker, &QueryWorker::queryToken, this, &AppController::onQueryToken);
    connect(m_activeQueryWorker, &QThread::finished, m_activeQueryWorker, &QObject::deleteLater);
    m_activeQueryWorker->start();
}

void AppController::refreshCorpusModel() {
    QVector<Corpus> corpora;
    if (m_engine->listCorpora(&corpora)) {
        m_corpusModel->setCorpora(corpora);
    }
}

void AppController::refreshChatSessionModel() {
    QVector<ChatSession> sessions;
    if (m_engine->listChatSessions(m_activeCorpusId, &sessions)) {
        m_chatSessionModel->setSessions(sessions);
    }
}

void AppController::refreshDocumentModel() {
    QVariantList stats;
    if (!m_engine->listDocumentStats(&stats)) {
        return;
    }
    QVector<DocumentEntry> documents;
    documents.reserve(stats.size());
    for (const QVariant &entryVar : stats) {
        const QVariantMap entry = entryVar.toMap();
        documents.append({entry.value(QStringLiteral("name")).toString(),
                          entry.value(QStringLiteral("passageCount")).toLongLong(),
                          entry.value(QStringLiteral("tokenCount")).toLongLong()});
    }
    m_documentModel->setDocuments(documents);
}

QVariantMap AppController::openDocument(const QString &documentName) {
    QVariantMap result;
    QString text;
    QVariantList chunks;
    if (!m_engine->getDocument(documentName, &text, &chunks)) {
        return result;
    }
    result[QStringLiteral("name")] = documentName;
    result[QStringLiteral("text")] = text;
    result[QStringLiteral("chunks")] = chunks;
    return result;
}

void AppController::removeDocument(const QString &documentName) {
    if (m_activeCorpusId < 0 || m_activeWorker != nullptr || m_activeCorpusId == m_ingestingCorpusId) {
        // No group open, or an ingest/rebuild is in flight for it -- the
        // removal writes to the live schema and would race the rebuild's
        // swap (same reasoning as deleteGroup()'s ingest guard).
        return;
    }
    if (!m_engine->removeDocument(documentName)) {
        emit notify(tr("Could not remove \"%1\": %2").arg(documentName, m_engine->lastError()));
        return;
    }
    refreshDocumentModel();
}
