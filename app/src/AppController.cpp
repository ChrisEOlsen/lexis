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
// Every path goes through the C core's paths module (dev-relative, or absolute once
// main.cpp calls lexis_paths_set() in a bundle). Conninfo/model path come from config.
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

    // Load immediately so the ~9-19s cost overlaps with first use (see ModelLoader.h).
    // Skipped when the model file isn't there yet; setup calls retryModelLoad() later.
    char *modelPath = config_load_model_path(lexis_paths_config_file());
    m_modelPath = QString::fromUtf8(modelPath);
    free(modelPath);
    if (QFileInfo::exists(m_modelPath)) {
        retryModelLoad();
    }

    // Settings state (see header). No explicit reranker call: the first query's lazy
    // init reads the config line itself.
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
    // Wait for in-flight workers before freeing the language data / model they use.
    if (m_activeWorker != nullptr) {
        // Cancel first so the wait ends at the next checkpoint (moments, not minutes).
        m_activeWorker->requestCancel();
        m_activeWorker->wait();
    }
    if (m_modelLoader != nullptr) {
        m_modelLoader->wait();
    }
    if (m_activeQueryWorker != nullptr) {
        m_activeQueryWorker->wait();
    }
    // Safe even if init failed or never ran (see local_llm_client.h).
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
        // Deleting mid-rebuild would race the rebuild's final schema swap.
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
        // Tear down chat state too: ON DELETE CASCADE already removed the sessions and
        // messages, so the models must not keep showing them.
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

    // Opening a group always lands on a fresh chat; resuming is explicit via the
    // history drawer (whose list must still reflect this group).
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

// m_last* must describe the session on screen; stale state would run one session's
// question against another session's id and overwrite an unrelated answer.
void AppController::clearLastExchange() {
    m_lastQuestion.clear();
    m_lastAnswerWasSearch = false;
    m_lastAnswer.clear();
    m_lastAnswerSources.clear();
    m_lastAnswerTool.clear();
    m_lastAnswerSearchQuery.clear();
    m_lastAnswerSearchTerms.clear();
}

// Re-derives the newest exchange from a history-loaded session so "Try harder" works
// there too. Only a trailing assistant message qualifies.
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
            // Two shapes: the current {"tool", "passages", ...} object, and the legacy bare
            // array (predates the CHAT path, so every such row came from search).
            if (doc.isObject()) {
                const QJsonObject root = doc.object();
                tool = root.value(QStringLiteral("tool")).toString();
                sources = root.value(QStringLiteral("passages")).toArray().toVariantList();
                // Missing keys yield empty strings, which is the QML hide condition.
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
        // No group selected, an ingest already running, or nothing dropped.
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
            // Dropped files go through as-is; IngestWorker reports unsupported ones as skipped.
            localPaths.append(localPath);
            continue;
        }
        // A dropped folder is walked recursively; only extractable types are kept (keep in
        // sync with IngestWorker's suffix dispatch). The rest is ignored silently.
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
        // A folder with nothing usable: say so, or the drop reads as not registering.
        emit notify(tr("No supported documents found in the dropped folder."));
        return;
    }

    m_busy = true;
    emit busyChanged();
    m_statusText = tr("Processing %1 file(s)...").arg(localPaths.size());
    emit statusTextChanged();

    // Chat with THIS group is blocked until the ingest lands (answers would come from a
    // half-built index). Every other group stays fully usable meanwhile.
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
        // Extraction: real per-file progress, scaled into the first 60% of the bar.
        m_ingestProgress = filesTotal > 0 ? 0.6 * double(filesDone) / double(filesTotal) : 0.0;
        m_ingestAnimMs = 250;
        m_ingestStatusText = tr("Reading documents (%1 of %2)...").arg(filesDone).arg(filesTotal);
    } else {
        // Index rebuild: hook-less C call, so QML animates to near-done over the estimate.
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
    m_activeWorker = nullptr; // object deletes itself via QThread::finished -> deleteLater()
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
        // Nothing landed: the temp schema was dropped, the group is unchanged.
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
        // No group, model not ready, a query already running, or an empty message.
        return;
    }
    if (m_activeCorpusId == m_ingestingCorpusId) {
        // Mid-rebuild: an answer would come from a half-built corpus. Backstop behind
        // ChatPanel's progress panel, in case a message slips through anyway.
        return;
    }

    if (m_activeChatSessionId < 0) {
        // First message of a new chat: create its session row now, titled from the
        // (truncated) question itself.
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
    connect(m_activeQueryWorker, &QueryWorker::queryFailed, this, &AppController::onQueryFailed);
    connect(m_activeQueryWorker, &QThread::finished, m_activeQueryWorker, &QObject::deleteLater);
    m_activeQueryWorker->start();
}

void AppController::onModelLoadFinished(bool ok) {
    m_modelLoader = nullptr; // deletes itself via QThread::finished -> deleteLater()
    m_modelReady = ok;
    emit modelReadyChanged();
    emit canRetryLastAnswerChanged();
    if (!ok) {
        emit notify(tr("Could not load the local model from %1.").arg(m_modelPath));
    }
}

void AppController::onQueryFinished(bool ok, QString answer, QVariantList sources, QString tool,
                                    QString searchQuery, QString searchTerms) {
    m_activeQueryWorker = nullptr; // deletes itself via QThread::finished -> deleteLater()
    m_chatBusy = false;
    emit chatBusyChanged();
    m_queryStageText.clear();
    emit queryStageTextChanged();

    if (!ok) {
        // A model failure explains itself; anything else gets the generic notice.
        QString failure = m_queryFailureReason;
        m_queryFailureReason.clear();
        if (m_retryingLiveAnswer) {
            // A failed retry must not destroy the answer it tried to improve: put it back.
            m_chatModel->restoreLastAssistant(m_lastAnswer, m_lastAnswerSources, m_lastAnswerTool,
                                              m_lastAnswerSearchQuery, m_lastAnswerSearchTerms);
            m_retryingLiveAnswer = false;
            // m_lastAnswerWasSearch untouched: the original is back, so a second retry must
            // still be offered (a failed query's empty tool would disable the button).
            emit canRetryLastAnswerChanged();
            emit notify(!failure.isEmpty()
                            ? failure
                            : tr("Couldn't improve that answer -- the original is unchanged."));
            return;
        } else {
            // A failed query leaves no trace: nothing was persisted, the live row goes too.
            m_chatModel->discardLive();
        }
        emit notify(!failure.isEmpty()
                        ? failure
                        : tr("Could not answer that question -- see the console for details."));
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
    // `answer` is always displayable on ok == true (already persisted by QueryWorker).
    // finishLive() finalizes the live row, or appends when no token ever arrived.
    m_chatModel->finishLive(answer, sources, tool, searchQuery, searchTerms);
    emit canRetryLastAnswerChanged();
}

void AppController::onQueryFailed(QString reason) {
    m_queryFailureReason = reason;
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
        // The streamed refusal text must be cleared: the retry regenerates from scratch.
        m_chatModel->resetLiveText();
        break;
    case StageSummarizing:
        m_queryStageText = tr("Summarizing the group...");
        // A fallback summary follows a streamed refusal or a partial read: clear it.
        // Harmless on the fresh path (no live row exists yet).
        m_chatModel->resetLiveText();
        break;
    case StageReadingDocuments:
        m_queryStageText = tr("Reading the documents...");
        // The fallback replaces a streamed refusal; harmless when nothing streamed.
        m_chatModel->resetLiveText();
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
    // No live call needed: each QueryWorker takes this as a per-query override.
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
    // Apply the live gate (off means the model is never even loaded).
    retrieval_set_reranker_enabled(enabled ? 1 : 0);
    m_rerankerEnabled = enabled;
    emit settingsChanged();
}

QString AppController::configDirectoryUrl() const {
    const QString configPath = QString::fromUtf8(lexis_paths_config_file());
    const QString dir = QFileInfo(configPath).absolutePath();
    return QUrl::fromLocalFile(dir).toString();
}

// False when a query is running, nothing was asked, the newest answer isn't SEARCH,
// or the group isn't answerable.
bool AppController::canRetryLastAnswer() const {
    return m_activeQueryWorker == nullptr && !m_lastQuestion.isEmpty() && m_lastAnswerWasSearch &&
           m_activeCorpusId >= 0 && m_modelReady && m_activeChatSessionId >= 0 &&
           m_activeCorpusId != m_ingestingCorpusId;
}

void AppController::retryLastAnswer() {
    if (!canRetryLastAnswer()) {
        return;
    }

    // Convert the newest answer's row into the live row up front, so the retry replaces
    // it in place (QueryWorker replaces the history row too).
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
    connect(m_activeQueryWorker, &QueryWorker::queryFailed, this, &AppController::onQueryFailed);
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
        // No group open, or a rebuild in flight (the removal would race its swap).
        return;
    }
    if (!m_engine->removeDocument(documentName)) {
        emit notify(tr("Could not remove \"%1\": %2").arg(documentName, m_engine->lastError()));
        return;
    }
    refreshDocumentModel();
}
