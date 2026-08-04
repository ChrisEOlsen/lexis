#include "MainWindow.h"
#include "GroupContentView.h"
#include "GroupSidebar.h"
#include "IngestWorker.h"
#include "LexisEngine.h"

extern "C" {
#include "csv_parse.h"
}

#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QSplitter>
#include <QTextStream>

namespace {
// Mirrors main.c's LEXIS_DB_CONNINFO/LEXIS_STOPWORDS_PATH/LEXIS_WORDNET_DIR
// exactly -- see that file's own comment on why the conninfo is never
// printed (embeds a password). No config UI exists yet for any of this
// to come from anywhere else.
const char *kConnInfo = "host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only";
const char *kStopwordsPath = "data/stopwords/english.txt";
const char *kWordnetDir = "data/wordnet";
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_engine(nullptr), m_sidebar(nullptr), m_content(nullptr), m_activeWorker(nullptr),
      m_activeCorpusId(-1), m_stopwords(nullptr), m_wordnet(nullptr), m_lemmatizer(nullptr) {
    setWindowTitle(tr("LEXIS"));
    resize(1000, 650);

    m_engine = std::make_unique<LexisEngine>(QString::fromUtf8(kConnInfo));
    if (!m_engine->isConnected()) {
        QMessageBox::critical(this, tr("LEXIS"),
                               tr("Could not connect to the database. Is Postgres running (make pg-start)?"));
    }

    m_stopwords = stopword_set_load(kStopwordsPath);
    m_wordnet = wordnet_table_load(kWordnetDir);
    m_lemmatizer = lemmatizer_load(kWordnetDir);
    if (m_stopwords == nullptr || m_wordnet == nullptr || m_lemmatizer == nullptr) {
        QMessageBox::critical(this, tr("LEXIS"),
                               tr("Could not load language data from data/stopwords or data/wordnet."));
    }

    m_sidebar = new GroupSidebar(m_engine.get(), this);
    m_content = new GroupContentView(m_engine.get(), this);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_sidebar);
    splitter->addWidget(m_content);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 780});

    setCentralWidget(splitter);

    connect(m_sidebar, &GroupSidebar::groupSelected, this, &MainWindow::onGroupSelected);
    connect(m_content, &GroupContentView::filesDropped, this, &MainWindow::onFilesDropped);
}

MainWindow::~MainWindow() {
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

void MainWindow::onGroupSelected(qint64 corpusId) {
    if (!m_engine->useCorpus(corpusId)) {
        QMessageBox::warning(this, tr("LEXIS"), tr("Could not switch groups: %1").arg(m_engine->lastError()));
        return;
    }
    m_activeCorpusId = corpusId;

    QVector<Corpus> corpora;
    m_engine->listCorpora(&corpora);
    QString displayName = QString::number(corpusId);
    for (const Corpus &corpus : corpora) {
        if (corpus.id == corpusId) {
            displayName = corpus.displayName;
            break;
        }
    }

    m_content->setActiveGroup(corpusId, displayName);
}

void MainWindow::onFilesDropped(QStringList localPaths) {
    if (m_activeCorpusId < 0 || m_activeWorker != nullptr) {
        // No group selected, or a previous ingest is still running --
        // GroupContentView disables itself (and therefore drops) while
        // busy, so the latter shouldn't normally be reachable; guarding
        // anyway rather than starting two concurrent rebuilds of the
        // same corpus.
        return;
    }

    QVector<QPair<QString, QString>> newDocuments;
    QStringList skipped;
    QStringList malformed;
    for (const QString &path : localPaths) {
        QFileInfo info(path);
        QString suffix = info.suffix().toLower();

        if (suffix == QStringLiteral("txt")) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                skipped.append(info.fileName());
                continue;
            }
            QTextStream stream(&file);
            newDocuments.append(qMakePair(info.fileName(), stream.readAll()));
        } else if (suffix == QStringLiteral("csv")) {
            // One CSV file can produce many documents -- one per data
            // row (see APP_SPEC.md's "CSV" section: v1's default
            // document mapping is one row = one document, every column
            // concatenated). csv_parse_file() fails the WHOLE file on
            // any malformed row rather than a partial parse, so a NULL
            // result here means the file is reported as malformed, not
            // silently skipped or partially ingested.
            TokenList *rows = csv_parse_file(path.toUtf8().constData());
            if (rows == nullptr) {
                malformed.append(info.fileName());
                continue;
            }
            for (size_t i = 0; i < rows->count; i++) {
                QString rowName = tr("%1#row%2").arg(info.fileName()).arg(i + 1);
                newDocuments.append(qMakePair(rowName, QString::fromUtf8(rows->terms[i])));
            }
            token_list_free(rows);
        } else {
            skipped.append(info.fileName());
        }
    }

    QStringList problems;
    if (!skipped.isEmpty()) {
        problems.append(tr("Not supported yet: %1").arg(skipped.join(QStringLiteral(", "))));
    }
    if (!malformed.isEmpty()) {
        problems.append(tr("Invalid CSV, not ingested: %1").arg(malformed.join(QStringLiteral(", "))));
    }
    if (!problems.isEmpty()) {
        QMessageBox::information(this, tr("LEXIS"), problems.join(QStringLiteral("\n\n")));
    }
    if (newDocuments.isEmpty()) {
        return;
    }

    m_content->setBusy(true, tr("Adding %1 document(s)...").arg(newDocuments.size()));

    m_activeWorker = new IngestWorker(QString::fromUtf8(kConnInfo), m_activeCorpusId, newDocuments, m_stopwords,
                                       m_wordnet, m_lemmatizer, this);
    connect(m_activeWorker, &IngestWorker::ingestFinished, this, &MainWindow::onIngestFinished);
    connect(m_activeWorker, &QThread::finished, m_activeWorker, &QObject::deleteLater);
    m_activeWorker->start();
}

void MainWindow::onIngestFinished(bool ok, qint64 totalPassages) {
    m_activeWorker = nullptr; // the object itself is cleaned up by the QThread::finished->deleteLater() connection

    if (!ok) {
        m_content->setBusy(false, tr("Ingestion failed -- see the console for details."));
        return;
    }

    m_content->setBusy(false, tr("Drag files here to add them to this group."));
    m_content->refresh();
    QMessageBox::information(this, tr("LEXIS"), tr("Group now has %1 total passages.").arg(totalPassages));
}
