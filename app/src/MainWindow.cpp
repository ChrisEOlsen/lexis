#include "MainWindow.h"
#include "GroupContentView.h"
#include "GroupSidebar.h"
#include "IngestWorker.h"
#include "LexisEngine.h"

#include <QMessageBox>
#include <QSplitter>

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
    if (m_activeCorpusId < 0 || m_activeWorker != nullptr || localPaths.isEmpty()) {
        // No group selected, a previous ingest is still running (
        // GroupContentView disables itself, and therefore drops, while
        // busy, so this shouldn't normally be reachable -- guarding
        // anyway rather than starting two concurrent rebuilds of the
        // same corpus), or nothing was actually dropped.
        return;
    }

    // Per-file extraction (CSV/DOCX/PDF/OCR) now happens inside
    // IngestWorker, on the background thread, alongside the database
    // rebuild -- see IngestWorker's own header comment for why: OCR
    // specifically is inherently slow, and running it here on the UI
    // thread before the worker even starts would freeze the UI exactly
    // the way the worker exists to prevent for the rebuild itself.
    m_content->setBusy(true, tr("Processing %1 file(s)...").arg(localPaths.size()));

    m_activeWorker = new IngestWorker(QString::fromUtf8(kConnInfo), m_activeCorpusId, localPaths, m_stopwords,
                                       m_wordnet, m_lemmatizer, this);
    connect(m_activeWorker, &IngestWorker::ingestFinished, this, &MainWindow::onIngestFinished);
    connect(m_activeWorker, &QThread::finished, m_activeWorker, &QObject::deleteLater);
    m_activeWorker->start();
}

void MainWindow::onIngestFinished(bool ok, qint64 totalPassages, QStringList skipped, QStringList malformed,
                                   QStringList noTextFound) {
    m_activeWorker = nullptr; // the object itself is cleaned up by the QThread::finished->deleteLater() connection

    m_content->setBusy(false, tr("Drag files here to add them to this group."));

    QStringList messageParts;
    if (!ok) {
        messageParts.append(tr("Ingestion failed -- see the console for details."));
    } else if (totalPassages > 0) {
        messageParts.append(tr("Group now has %1 total passages.").arg(totalPassages));
        m_content->refresh();
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
        QMessageBox::information(this, tr("LEXIS"), messageParts.join(QStringLiteral("\n\n")));
    }
}
