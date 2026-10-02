// One chat query on a background thread: route -> search/summary/chat pipeline -> persist.
// Own PgStore connection; caller must never run two QueryWorkers (or a ModelLoader) at once.

#ifndef LEXIS_APP_QUERYWORKER_H
#define LEXIS_APP_QUERYWORKER_H

#include <QString>
#include <QThread>
#include <QVariantList>

extern "C" {
#include "lemmatizer.h"
#include "local_llm_client.h"
#include "stopwords.h"
#include "wordnet.h"
}

// Pipeline stages, reported as they begin (a bare "Thinking..." over seconds reads as a hang).
// Retrying replaces a second Writing so the UI can clear the streamed refusal text.
enum QueryStage {
    StageRouting = 0,
    StageRewriting,
    StageSearching,
    StageReading,
    StageWriting,
    StageRetrying,
    StageSummarizing,
    // Appended at the end so the existing values stay stable.
    StageReadingDocuments,
};

class QueryWorker : public QThread {
    Q_OBJECT

public:
    // forceRetry: "Try harder" -- deeper retrieval + forced reasoning, no re-routing.
    // thinkingOverride: -1 follows config, 0/1 force off/on (Settings applies live).
    QueryWorker(QString conninfo, qint64 corpusId, qint64 sessionId, QString question,
                const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                bool forceRetry = false, int thinkingOverride = -1, QObject *parent = nullptr);

signals:
    // Named queryFinished, not finished (QThread::finished would be shadowed).
    // `answer` is never empty on ok == true; `tool`/`sources`/provenance feed the inspector.
    void queryFinished(bool ok, QString answer, QVariantList sources, QString tool,
                       QString searchQuery, QString searchTerms);

    // Progress from the worker thread (queued). Token pieces concatenate to the answer.
    void queryStage(int stage, int payload);
    void queryToken(QString piece);
    // Fired just before queryFinished(false,...) when the model itself went quiet
    // (routing or generation got no response), so the UI can say so specifically.
    void queryFailed(QString reason);

protected:
    void run() override;

private:
    QString m_conninfo;
    qint64 m_corpusId;
    qint64 m_sessionId;
    QString m_question;
    const StopwordSet *m_stopwords;
    const WordNetTable *m_wordnet;
    const Lemmatizer *m_lemmatizer;
    bool m_forceRetry;
    int m_thinkingOverride;
};

#endif // LEXIS_APP_QUERYWORKER_H
