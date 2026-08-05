// Thin adapter over pg_store.c's corpus-management C API: owns one
// PgStore connection, translates its malloc/free-and-return-code
// convention into Qt types (QString, QVector) and a retrievable error
// string. UI code should go through this, not call pg_store_* directly
// -- see ../../APP_SPEC.md's "Engine integration" discussion for why
// (keeps memory management and error handling in one place instead of
// every widget that touches the engine).
//
// Corpus CRUD here is synchronous -- fast, single-round-trip SQL, safe
// to call straight from the UI thread. Ingestion (bulk_ingest_tsv() /
// bulk_ingest_rebuild_corpus()) is a different story -- long-running,
// needs its own worker-thread treatment -- and deliberately isn't this
// class's job; it'll get its own adapter when that piece is built.

#ifndef LEXIS_APP_LEXISENGINE_H
#define LEXIS_APP_LEXISENGINE_H

#include <QString>
#include <QVector>

extern "C" {
#include "pg_store.h"
}

// Qt-idiomatic mirror of PgStoreCorpus -- a value type callers can copy
// freely, unlike the C struct's malloc'd array (see
// pg_store_corpora_free()).
struct Corpus {
    qint64 id;
    QString displayName;
};

// Qt-idiomatic mirror of PgStoreChatSession.
struct ChatSession {
    qint64 id;
    QString title;
};

// Qt-idiomatic mirror of PgStoreChatMessage. Named ChatHistoryEntry, not
// ChatMessage -- ChatMessageListModel.h already has a ChatMessage struct
// of its own, shaped for QML display (sources as a QVariantList, already
// parsed) rather than persistence (sourcesJson as a raw string, this
// module never parses it -- see pg_store.h's chat_messages comment).
// sourcesJson is empty for a user message.
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

    // False if the constructor's pg_store_open() failed (e.g. Postgres
    // isn't running) -- every other method below is a safe no-op
    // (returns false, sets lastError()) when this is false, rather than
    // crashing on a null connection.
    bool isConnected() const;

    // Human-readable detail for the most recent failed call on this
    // object. Meaningless after a call that returned true.
    QString lastError() const;

    // Creates a new group. On success, *idOut (if non-null) is set to
    // the new corpus's id. Returns false on failure (including an
    // empty/whitespace-only displayName, rejected client-side before
    // ever reaching the database).
    bool createCorpus(const QString &displayName, qint64 *idOut = nullptr);

    // Replaces *out with every registered group, oldest first. Returns
    // false on failure (*out left empty, not partially filled).
    bool listCorpora(QVector<Corpus> *out);

    // Scopes every subsequent engine call (once ingestion/search methods
    // exist here) to corpusId's schema. Returns false if corpusId
    // doesn't exist.
    bool useCorpus(qint64 corpusId);

    // Permanently deletes a group and everything in it. Returns false if
    // corpusId doesn't exist or the deletion fails.
    bool deleteCorpus(qint64 corpusId);

    // Lists every document's name in whichever corpus useCorpus() last
    // scoped this connection to -- reads through that same scoped
    // connection, doesn't take a corpus id of its own. Returns false on
    // failure (*out left empty, not partially filled).
    bool listDocumentNames(QVector<QString> *out);

    // -- Chat sessions -- take corpusId/sessionId directly as SQL
    // parameters rather than relying on useCorpus()'s search_path, since
    // chat_sessions/chat_messages live permanently in the public schema,
    // not inside any corpus's own per-corpus schema (see pg_store.h's
    // "Chat history" comment for why -- rebuild-on-append would silently
    // destroy them otherwise). --

    // Creates a new chat session under corpusId, titled `title`. On
    // success, *idOut (if non-null) is set to the new session's id.
    // Returns false on failure (including an empty/whitespace-only
    // title, rejected client-side before ever reaching the database).
    bool createChatSession(qint64 corpusId, const QString &title, qint64 *idOut = nullptr);

    // Replaces *out with every chat session under corpusId, newest
    // first. Returns false on failure (*out left empty, not partially
    // filled).
    bool listChatSessions(qint64 corpusId, QVector<ChatSession> *out);

    // Permanently deletes a chat session and every message in it.
    // Returns false if sessionId doesn't exist or the deletion fails.
    bool deleteChatSession(qint64 sessionId);

    // Replaces *out with every message in sessionId, oldest first.
    // Returns false on failure (*out left empty, not partially filled).
    bool getChatMessages(qint64 sessionId, QVector<ChatHistoryEntry> *out);

private:
    // Pulls the real Postgres error text via PQerrorMessage(m_store->conn)
    // -- PgStore's conn field is public specifically so callers can do
    // this, rather than pg_store.c needing its own last-error tracking
    // just to serve this adapter. Falls back to `context` if libpq has
    // nothing (e.g. a client-side precondition failure that never
    // touched the connection).
    void captureError(const char *context);

    PgStore *m_store;
    QString m_lastError;
};

#endif // LEXIS_APP_LEXISENGINE_H
