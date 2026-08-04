#include "AppController.h"
#include "CorpusListModel.h"
#include "DocumentListModel.h"
#include "IngestWorker.h"
#include "LexisEngine.h"

#include <QUrl>

namespace {
// Mirrors main.c's LEXIS_DB_CONNINFO/LEXIS_STOPWORDS_PATH/LEXIS_WORDNET_DIR
// exactly -- see that file's own comment on why the conninfo is never
// printed (embeds a password). No config UI exists yet for any of this
// to come from anywhere else.
const char *kConnInfo = "host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only";
const char *kStopwordsPath = "data/stopwords/english.txt";
const char *kWordnetDir = "data/wordnet";
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent), m_engine(nullptr), m_corpusModel(new CorpusListModel(this)),
      m_documentModel(new DocumentListModel(this)), m_activeWorker(nullptr), m_activeCorpusId(-1), m_busy(false),
      m_statusText(tr("Select a group")), m_stopwords(nullptr), m_wordnet(nullptr), m_lemmatizer(nullptr) {
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
}

AppController::~AppController() {
    // A rebuild is a real database operation already in progress on
    // another thread -- waiting for it here (blocking app close briefly)
    // is the correct, safe behavior; destroying the language data below
    // out from under a still-running worker would not be.
    if (m_activeWorker != nullptr) {
        m_activeWorker->wait();
    }
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
    emit activeCorpusIdChanged();
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

void AppController::refreshCorpusModel() {
    QVector<Corpus> corpora;
    if (m_engine->listCorpora(&corpora)) {
        m_corpusModel->setCorpora(corpora);
    }
}

void AppController::refreshDocumentModel() {
    QVector<QString> names;
    if (m_engine->listDocumentNames(&names)) {
        m_documentModel->setDocumentNames(names);
    }
}
