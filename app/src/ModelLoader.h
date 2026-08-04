// Loads the local GGUF model exactly once, on a background thread --
// local_llm_client_init() blocks for roughly 9-19 seconds (measured
// directly, see SPEED.md/LIMITATIONS.md), so it can't run on the UI
// thread. AppController kicks this off proactively as soon as the app
// starts, not deferred to the first chat message -- the cost then
// overlaps with whatever the user does first (browsing groups,
// ingesting documents) instead of stalling their first question.

#ifndef LEXIS_APP_MODELLOADER_H
#define LEXIS_APP_MODELLOADER_H

#include <QString>
#include <QThread>

class ModelLoader : public QThread {
    Q_OBJECT

public:
    explicit ModelLoader(QString modelPath, QObject *parent = nullptr);

signals:
    // Named modelLoadFinished, not finished -- same QThread::finished()
    // shadowing reason as IngestWorker::ingestFinished.
    void modelLoadFinished(bool ok);

protected:
    void run() override;

private:
    QString m_modelPath;
};

#endif // LEXIS_APP_MODELLOADER_H
