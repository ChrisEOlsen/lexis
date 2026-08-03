#include "MainWindow.h"
#include "GroupSidebar.h"
#include "LexisEngine.h"

#include <QLabel>
#include <QMessageBox>
#include <QSplitter>

namespace {
// Mirrors main.c's LEXIS_DB_CONNINFO exactly -- see that file's own
// comment on why it's never printed (embeds a password). No config UI
// exists yet for this to come from anywhere else.
const char *kConnInfo = "host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only";
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_engine(nullptr), m_sidebar(nullptr), m_content(nullptr) {
    setWindowTitle(tr("LEXIS"));
    resize(1000, 650);

    m_engine = std::make_unique<LexisEngine>(QString::fromUtf8(kConnInfo));
    if (!m_engine->isConnected()) {
        QMessageBox::critical(this, tr("LEXIS"),
                               tr("Could not connect to the database. Is Postgres running (make pg-start)?"));
    }

    m_sidebar = new GroupSidebar(m_engine.get(), this);
    m_content = new QLabel(tr("Select a group"), this);
    m_content->setAlignment(Qt::AlignCenter);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_sidebar);
    splitter->addWidget(m_content);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 780});

    setCentralWidget(splitter);

    connect(m_sidebar, &GroupSidebar::groupSelected, this, &MainWindow::onGroupSelected);
}

MainWindow::~MainWindow() = default;

void MainWindow::onGroupSelected(qint64 corpusId) {
    if (!m_engine->useCorpus(corpusId)) {
        QMessageBox::warning(this, tr("LEXIS"), tr("Could not switch groups: %1").arg(m_engine->lastError()));
        return;
    }
    m_content->setText(tr("Group %1 selected").arg(corpusId));
}
