// Qt adapter over pg_store.c: owns one PgStore connection, returns Qt types + lastError().
// Synchronous single-round-trip SQL only; ingestion lives in IngestWorker instead.

#ifndef LEXIS_APP_LEXISENGINE_H
#define LEXIS_APP_LEXISENGINE_H

#include <QDateTime>
#include <QString>
#include <QVariantList>
#include <QVector>

extern "C" {
#include "pg_store.h"
}

// Copyable mirror of PgStoreCorpus (unlike its malloc'd array).
struct Corpus {
    qint64 id;
    QString displayName;
};

// Copyable mirror of PgStoreChatSession; createdAt parsed via Qt::ISODate.
struct ChatSession {
    qint64 id;
    QString title;
    QDateTime createdAt;
};

// Mirror of PgStoreChatMessage; sourcesJson stays raw (never parsed here).
struct ChatHistoryEntry {
    bool isUser;
    QString text;
    QString sourcesJson;
};

class LexisEngine {
public:
    explicit LexisEngine(const QString &conninfo);
    ~LexisEngine();

    LexisEngine(const LexisEngine &) = delete;
    LexisEngine &operator=(const LexisEngine &) = delete;

    // False when pg_store_open() failed; every other method is then a safe no-op.
    bool isConnected() const;

    // Human-readable detail for the most recent failed call on this
    // object. Meaningless after a call that returned true.
    QString lastError() const;

    // Creates a group (*idOut gets its id); empty names are rejected client-side.
    bool createCorpus(const QString &displayName, qint64 *idOut = nullptr);

    // Every group, oldest first; *out left empty (not partial) on failure.
    bool listCorpora(QVector<Corpus> *out);

    // Scopes subsequent calls to corpusId's schema. False if it doesn't exist.
    bool useCorpus(qint64 corpusId);

    // Permanently deletes a group and everything in it. Returns false if
    // corpusId doesn't exist or the deletion fails.
    bool deleteCorpus(qint64 corpusId);

    // -- Per-document reads/removal, on the corpus useCorpus() last selected. --

    // Viewer payload: full text plus {"chunkId", "text", "tokenCount"} maps in order.
    bool getDocument(const QString &documentName, QString *textOut, QVariantList *chunksOut);

    // Per-document {name, passageCount, tokenCount} maps for the
    // document list, one round trip.
    bool listDocumentStats(QVariantList *out);

    // Removes a document transactionally (nothing changes on failure).
    bool removeDocument(const QString &documentName);

    // -- Chat sessions (public schema, so ids are SQL params, not search_path). --

    // Creates a chat session under corpusId (*idOut gets its id).
    bool createChatSession(qint64 corpusId, const QString &title, qint64 *idOut = nullptr);

    // Every chat session under corpusId, newest first.
    bool listChatSessions(qint64 corpusId, QVector<ChatSession> *out);

    // Permanently deletes a chat session and every message in it.
    bool deleteChatSession(qint64 sessionId);

    // Every message in sessionId, oldest first.
    bool getChatMessages(qint64 sessionId, QVector<ChatHistoryEntry> *out);

private:
    // Real Postgres error text via PQerrorMessage, falling back to `context`.
    void captureError(const char *context);

    PgStore *m_store;
    QString m_lastError;
};

#endif // LEXIS_APP_LEXISENGINE_H
