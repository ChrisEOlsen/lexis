// The one object QML talks to for everything backend-related: groups,
// documents, ingestion. Wraps LexisEngine (the actual pg_store C-API
// adapter) plus the two list models QML binds its ListViews to. Exposed
// as a QML singleton -- QML creates and owns the single instance
// automatically the first time it's referenced after `import Lexis`, no
// manual registration needed in main.cpp.
//
// Owns the language data (stopwords/wordnet/lemmatizer) every ingest
// needs, loaded once for the app's whole lifetime and shared read-only
// across every IngestWorker -- see IngestWorker.h's own comment on why
// that's safe.

#ifndef LEXIS_APP_APPCONTROLLER_H
#define LEXIS_APP_APPCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>

#include <memory>

extern "C" {
#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"
}

// Full definitions, not forward declarations -- Q_PROPERTY's pointer
// types need the complete class visible for Qt's meta-type
// registration (MOC-generated code fails a static_assert otherwise).
#include "CorpusListModel.h"
#include "DocumentListModel.h"

class LexisEngine;
class IngestWorker;

class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool connected READ isConnected CONSTANT)
    Q_PROPERTY(qint64 activeCorpusId READ activeCorpusId NOTIFY activeCorpusIdChanged)
    Q_PROPERTY(QString activeCorpusName READ activeCorpusName NOTIFY activeCorpusIdChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(CorpusListModel *corpusModel READ corpusModel CONSTANT)
    Q_PROPERTY(DocumentListModel *documentModel READ documentModel CONSTANT)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    bool isConnected() const;
    qint64 activeCorpusId() const;
    QString activeCorpusName() const;
    bool isBusy() const;
    QString statusText() const;
    CorpusListModel *corpusModel() const;
    DocumentListModel *documentModel() const;

    Q_INVOKABLE bool createGroup(const QString &displayName);
    Q_INVOKABLE bool deleteGroup(qint64 corpusId);
    Q_INVOKABLE void selectGroup(qint64 corpusId);

    // fileUrls are raw file:// URL strings straight from QML's
    // DropArea.drop.urls -- converted to local paths here (via
    // QUrl::toLocalFile(), not string manipulation) rather than in QML,
    // since that's the robust way to handle URL-encoded characters
    // (spaces, non-ASCII filenames) correctly.
    Q_INVOKABLE void ingestFiles(const QStringList &fileUrls);

signals:
    void activeCorpusIdChanged();
    void busyChanged();
    void statusTextChanged();
    // Reused for both real errors and informational results (e.g. "N
    // passages added") -- QML shows both the same way, as a dismissible
    // message dialog; splitting into two signals would just double the
    // QML-side wiring for no behavioral difference.
    void notify(QString message);

private slots:
    void onIngestFinished(bool ok, qint64 totalPassages, QStringList skipped, QStringList malformed,
                           QStringList noTextFound);

private:
    void refreshCorpusModel();
    void refreshDocumentModel();

    std::unique_ptr<LexisEngine> m_engine;
    CorpusListModel *m_corpusModel;
    DocumentListModel *m_documentModel;
    IngestWorker *m_activeWorker; // non-owning; deletes itself via QThread::finished -> deleteLater()

    qint64 m_activeCorpusId;
    QString m_activeCorpusName;
    bool m_busy;
    QString m_statusText;

    StopwordSet *m_stopwords;
    WordNetTable *m_wordnet;
    Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_APPCONTROLLER_H
