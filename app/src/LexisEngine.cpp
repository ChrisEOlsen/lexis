#include "LexisEngine.h"

#include <cstdlib>

LexisEngine::LexisEngine(const QString &conninfo) : m_store(nullptr) {
    m_store = pg_store_open(conninfo.toUtf8().constData());
    if (m_store == nullptr) {
        m_lastError = QStringLiteral("Could not connect to the database.");
    }
}

LexisEngine::~LexisEngine() {
    pg_store_close(m_store);
}

bool LexisEngine::isConnected() const {
    return m_store != nullptr;
}

QString LexisEngine::lastError() const {
    return m_lastError;
}

void LexisEngine::captureError(const char *context) {
    QString detail = QString::fromUtf8(PQerrorMessage(m_store->conn)).trimmed();
    m_lastError = detail.isEmpty() ? QString::fromUtf8(context) : detail;
}

bool LexisEngine::createCorpus(const QString &displayName, qint64 *idOut) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }
    if (displayName.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("Group name cannot be empty.");
        return false;
    }

    char *schemaName = nullptr;
    int64_t id = pg_store_create_corpus(m_store, displayName.toUtf8().constData(), &schemaName);
    if (id <= 0) {
        captureError("Failed to create group.");
        return false;
    }
    free(schemaName); // opaque, deliberately unused here -- see pg_store_create_corpus()'s doc comment

    if (idOut != nullptr) {
        *idOut = static_cast<qint64>(id);
    }
    return true;
}

bool LexisEngine::listCorpora(QVector<Corpus> *out) {
    out->clear();
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }

    size_t count = 0;
    PgStoreCorpus *corpora = pg_store_list_corpora(m_store, &count);
    if (corpora == nullptr) {
        captureError("Failed to list groups.");
        return false;
    }

    out->reserve(static_cast<int>(count));
    for (size_t i = 0; i < count; i++) {
        out->append(Corpus{static_cast<qint64>(corpora[i].id), QString::fromUtf8(corpora[i].display_name)});
    }
    pg_store_corpora_free(corpora, count);
    return true;
}

bool LexisEngine::useCorpus(qint64 corpusId) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }
    if (pg_store_use_corpus(m_store, corpusId) != 0) {
        captureError("Failed to switch groups.");
        return false;
    }
    return true;
}

bool LexisEngine::deleteCorpus(qint64 corpusId) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }
    if (pg_store_delete_corpus(m_store, corpusId) != 0) {
        captureError("Failed to delete group.");
        return false;
    }
    return true;
}

bool LexisEngine::getDocument(const QString &documentName, QString *textOut, QVariantList *chunksOut) {
    if (textOut == nullptr || chunksOut == nullptr) {
        return false;
    }
    textOut->clear();
    chunksOut->clear();
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }

    char *text = pg_store_get_document_text(m_store, documentName.toUtf8().constData());
    if (text == nullptr) {
        captureError("Failed to read document.");
        return false;
    }
    *textOut = QString::fromUtf8(text);
    free(text);

    size_t chunk_count = 0;
    PgStoreDocumentPassage *chunks =
        pg_store_get_document_passages(m_store, documentName.toUtf8().constData(), &chunk_count);
    if (chunks == nullptr) {
        captureError("Failed to read document passages.");
        return false;
    }
    for (size_t i = 0; i < chunk_count; i++) {
        QVariantMap chunk;
        chunk[QStringLiteral("chunkId")] = chunks[i].chunk_id;
        chunk[QStringLiteral("text")] = QString::fromUtf8(chunks[i].text);
        chunk[QStringLiteral("tokenCount")] = chunks[i].token_count;
        chunksOut->append(chunk);
    }
    pg_store_document_passages_free(chunks, chunk_count);
    return true;
}

bool LexisEngine::listDocumentStats(QVariantList *out) {
    out->clear();
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }

    size_t count = 0;
    PgStoreDocumentStats *stats = pg_store_list_document_stats(m_store, &count);
    if (stats == nullptr) {
        captureError("Failed to list document stats.");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        QVariantMap entry;
        entry[QStringLiteral("name")] = QString::fromUtf8(stats[i].document_name);
        entry[QStringLiteral("passageCount")] = static_cast<qlonglong>(stats[i].passage_count);
        entry[QStringLiteral("tokenCount")] = static_cast<qlonglong>(stats[i].total_tokens);
        out->append(entry);
    }
    pg_store_document_stats_free(stats, count);
    return true;
}

bool LexisEngine::removeDocument(const QString &documentName) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }
    if (pg_store_remove_document(m_store, documentName.toUtf8().constData()) != 0) {
        captureError("Failed to remove document.");
        return false;
    }
    return true;
}

bool LexisEngine::createChatSession(qint64 corpusId, const QString &title, qint64 *idOut) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }
    if (title.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("Chat session title cannot be empty.");
        return false;
    }

    int64_t id = pg_store_create_chat_session(m_store, corpusId, title.toUtf8().constData());
    if (id <= 0) {
        captureError("Failed to create chat session.");
        return false;
    }

    if (idOut != nullptr) {
        *idOut = static_cast<qint64>(id);
    }
    return true;
}

bool LexisEngine::listChatSessions(qint64 corpusId, QVector<ChatSession> *out) {
    out->clear();
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }

    size_t count = 0;
    PgStoreChatSession *sessions = pg_store_list_chat_sessions(m_store, corpusId, &count);
    if (sessions == nullptr) {
        captureError("Failed to list chat sessions.");
        return false;
    }

    out->reserve(static_cast<int>(count));
    for (size_t i = 0; i < count; i++) {
        QDateTime createdAt = QDateTime::fromString(QString::fromUtf8(sessions[i].created_at), Qt::ISODate);
        out->append(ChatSession{static_cast<qint64>(sessions[i].id), QString::fromUtf8(sessions[i].title), createdAt});
    }
    pg_store_chat_sessions_free(sessions, count);
    return true;
}

bool LexisEngine::deleteChatSession(qint64 sessionId) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }
    if (pg_store_delete_chat_session(m_store, sessionId) != 0) {
        captureError("Failed to delete chat session.");
        return false;
    }
    return true;
}

bool LexisEngine::getChatMessages(qint64 sessionId, QVector<ChatHistoryEntry> *out) {
    out->clear();
    if (!isConnected()) {
        m_lastError = QStringLiteral("Not connected to the database.");
        return false;
    }

    size_t count = 0;
    PgStoreChatMessage *messages = pg_store_get_chat_messages(m_store, sessionId, &count);
    if (messages == nullptr) {
        captureError("Failed to load chat history.");
        return false;
    }

    out->reserve(static_cast<int>(count));
    for (size_t i = 0; i < count; i++) {
        out->append(ChatHistoryEntry{
            messages[i].is_user != 0, QString::fromUtf8(messages[i].text),
            messages[i].sources_json != nullptr ? QString::fromUtf8(messages[i].sources_json) : QString()});
    }
    pg_store_chat_messages_free(messages, count);
    return true;
}
