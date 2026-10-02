// Loads the local GGUF once, on a background thread (init blocks ~9-19s).
// Started proactively at launch so the cost overlaps with first use.

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
