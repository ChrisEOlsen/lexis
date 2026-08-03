#include "GroupContentView.h"
#include "LexisEngine.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFont>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QUrl>
#include <QVBoxLayout>

GroupContentView::GroupContentView(LexisEngine *engine, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_activeCorpusId(-1), m_titleLabel(nullptr), m_statusLabel(nullptr),
      m_documentList(nullptr) {
    setAcceptDrops(true);

    m_titleLabel = new QLabel(tr("Select a group"), this);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);

    m_statusLabel = new QLabel(tr("Select a group to see its documents."), this);
    m_statusLabel->setWordWrap(true);

    m_documentList = new QListWidget(this);
    m_documentList->setEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_titleLabel);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_documentList);
}

void GroupContentView::setActiveGroup(qint64 corpusId, const QString &displayName) {
    m_activeCorpusId = corpusId;
    m_titleLabel->setText(displayName);
    m_documentList->setEnabled(true);
    m_statusLabel->setText(tr("Drag files here to add them to this group."));
    refresh();
}

void GroupContentView::clearActiveGroup() {
    m_activeCorpusId = -1;
    m_titleLabel->setText(tr("Select a group"));
    m_statusLabel->setText(tr("Select a group to see its documents."));
    m_documentList->clear();
    m_documentList->setEnabled(false);
}

void GroupContentView::refresh() {
    m_documentList->clear();
    if (m_activeCorpusId < 0) {
        return;
    }

    QVector<QString> names;
    if (!m_engine->listDocumentNames(&names)) {
        QMessageBox::warning(this, tr("LEXIS"), tr("Could not load documents: %1").arg(m_engine->lastError()));
        return;
    }
    for (const QString &name : names) {
        m_documentList->addItem(name);
    }
}

void GroupContentView::setBusy(bool busy, const QString &statusText) {
    setEnabled(!busy);
    if (!statusText.isEmpty()) {
        m_statusLabel->setText(statusText);
    }
}

void GroupContentView::dragEnterEvent(QDragEnterEvent *event) {
    if (m_activeCorpusId >= 0 && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void GroupContentView::dropEvent(QDropEvent *event) {
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (!paths.isEmpty()) {
        emit filesDropped(paths);
    }
}
