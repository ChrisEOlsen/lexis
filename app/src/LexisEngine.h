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
