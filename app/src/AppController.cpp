#include "AppController.h"
#include "CorpusListModel.h"
#include "DocumentListModel.h"
#include "IngestWorker.h"
#include "LexisEngine.h"
#include "ModelLoader.h"
#include "QueryWorker.h"

extern "C" {
#include "local_llm_client.h"
}

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

namespace {
// Mirrors main.c's LEXIS_DB_CONNINFO/LEXIS_STOPWORDS_PATH/LEXIS_WORDNET_DIR/
// LEXIS_MODEL_PATH exactly -- see that file's own comment on why the
// conninfo is never printed (embeds a password). No config UI exists
// yet for any of this to come from anywhere else.
const char *kConnInfo = "host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only";
const char *kStopwordsPath = "data/stopwords/english.txt";
const char *kWordnetDir = "data/wordnet";
const char *kModelPath = "data/models/gemma-4-E2B-it-Q4_K_M.gguf"; // see main.c's LEXIS_MODEL_PATH comment
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent), m_engine(nullptr), m_corpusModel(new CorpusListModel(this)),
      m_documentModel(new DocumentListModel(this)), m_chatModel(new ChatMessageListModel(this)),
      m_chatSessionModel(new ChatSessionListModel(this)), m_activeWorker(nullptr), m_modelLoader(nullptr),
      m_activeQueryWorker(nullptr), m_activeCorpusId(-1), m_activeChatSessionId(-1),
      m_activeChatSessionTitle(tr("New Chat")), m_busy(false), m_statusText(tr("Select a group")),
      m_modelReady(false), m_chatBusy(false), m_stopwords(nullptr), m_wordnet(nullptr), m_lemmatizer(nullptr) {
    m_engine = std::make_unique<LexisEngine>(QString::fromUtf8(kConnInfo));
    if (!m_engine->isConnected()) {
        emit notify(tr("Could not connect to the database. Is Postgres running (make pg-start)?"));
    } else {
        refreshCorpusModel();
    }

    m_stopwords = stopword_set_load(kStopwordsPath);
    m_wordnet = wordnet_table_load(kWordnetDir);
    m_lemmatizer = lemmatizer_load(kWordnetDir);
    if (m_stopwords == nullptr || m_wordnet == nullptr || m_lemmatizer == nullptr) {
        emit notify(tr("Could not load language data from data/stopwords or data/wordnet."));
    }

    // Kicked off immediately, not deferred to first chat use -- see
    // ModelLoader.h's own comment on why (~9-19s load time overlapping
    // with whatever the user does first, instead of stalling their
    // first question).
    m_modelLoader = new ModelLoader(QString::fromUtf8(kModelPath), this);
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
    if (!m_engine->deleteCorpus(corpusId)) {
        emit notify(tr("Could not delete group: %1").arg(m_engine->lastError()));
        return false;
    }
    if (corpusId == m_activeCorpusId) {
        m_activeCorpusId = -1;
        m_activeCorpusName.clear();
        m_documentModel->setDocumentNames({});
        emit activeCorpusIdChanged();
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

    // Pick up where the user left off: auto-select the most recent
    // session for this group (LexisEngine::listChatSessions() returns
    // newest first), or fall back to startNewChat()'s pending state if
    // the group has no chat history yet.
    QVector<ChatSession> sessions;
    m_engine->listChatSessions(corpusId, &sessions);
    m_chatSessionModel->setSessions(sessions);
    if (!sessions.isEmpty()) {
        selectChatSession(sessions.first().id);
    } else {
        startNewChat();
    }

    emit activeCorpusIdChanged();
}

void AppController::startNewChat() {
    m_activeChatSessionId = -1;
    m_activeChatSessionTitle = tr("New Chat");
    m_chatModel->setMessages({});
    emit activeChatSessionIdChanged();
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
        if (!entry.sourcesJson.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(entry.sourcesJson.toUtf8());
            if (doc.isArray()) {
                sources = doc.array().toVariantList();
            }
        }
        messages.append(ChatMessage{entry.text, entry.isUser, sources, /*isFresh=*/false});
    }
    m_chatModel->setMessages(messages);

    emit activeChatSessionIdChanged();
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
        if (!localPath.isEmpty()) {
            localPaths.append(localPath);
        }
    }
    if (localPaths.isEmpty()) {
        return;
    }

    m_busy = true;
    emit busyChanged();
    m_statusText = tr("Processing %1 file(s)...").arg(localPaths.size());
    emit statusTextChanged();

    m_activeWorker = new IngestWorker(QString::fromUtf8(kConnInfo), m_activeCorpusId, localPaths, m_stopwords,
                                       m_wordnet, m_lemmatizer, this);
    connect(m_activeWorker, &IngestWorker::ingestFinished, this, &AppController::onIngestFinished);
    connect(m_activeWorker, &QThread::finished, m_activeWorker, &QObject::deleteLater);
    m_activeWorker->start();
}

void AppController::onIngestFinished(bool ok, qint64 totalPassages, QStringList skipped, QStringList malformed,
                                      QStringList noTextFound) {
    m_activeWorker = nullptr; // the object itself is cleaned up by the QThread::finished->deleteLater() connection
    m_busy = false;
    m_statusText = tr("Drag files here to add them to this group.");
    emit busyChanged();
    emit statusTextChanged();

    if (ok && totalPassages > 0) {
        refreshDocumentModel();
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

    m_activeQueryWorker = new QueryWorker(QString::fromUtf8(kConnInfo), m_activeCorpusId, m_activeChatSessionId,
                                           question, m_stopwords, m_wordnet, m_lemmatizer, this);
    connect(m_activeQueryWorker, &QueryWorker::queryFinished, this, &AppController::onQueryFinished);
    connect(m_activeQueryWorker, &QThread::finished, m_activeQueryWorker, &QObject::deleteLater);
    m_activeQueryWorker->start();
}

void AppController::onModelLoadFinished(bool ok) {
    m_modelLoader = nullptr; // cleaned up by the QThread::finished->deleteLater() connection
    m_modelReady = ok;
    emit modelReadyChanged();
    if (!ok) {
        emit notify(tr("Could not load the local model from %1.").arg(QString::fromUtf8(kModelPath)));
    }
}

void AppController::onQueryFinished(bool ok, QString answer, QVariantList sources) {
    m_activeQueryWorker = nullptr; // cleaned up by the QThread::finished->deleteLater() connection
    m_chatBusy = false;
    emit chatBusyChanged();

    if (!ok) {
        emit notify(tr("Could not answer that question -- see the console for details."));
        return;
    }
    // QueryWorker always sends a real, displayable answer on ok == true
    // now -- "nothing to search for" and "no matching passages" are
    // themselves the answer text (and already persisted to
    // chat_messages by QueryWorker), not a sentinel this slot has to
    // special-case into a synthesized message of its own; doing that
    // here would desync what's shown live from what's actually stored.
    m_chatModel->addMessage(answer, false, sources);
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
    QVector<QString> names;
    if (m_engine->listDocumentNames(&names)) {
        m_documentModel->setDocumentNames(names);
    }
}
