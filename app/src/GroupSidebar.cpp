#include "GroupSidebar.h"
#include "LexisEngine.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
// Where each QListWidgetItem stashes its corpus id -- the list shows
// display names, but every engine call needs the id.
constexpr int CorpusIdRole = Qt::UserRole;
} // namespace

GroupSidebar::GroupSidebar(LexisEngine *engine, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_list(nullptr), m_newButton(nullptr), m_deleteButton(nullptr) {
    m_list = new QListWidget(this);
    m_newButton = new QPushButton(tr("New Group"), this);
    m_deleteButton = new QPushButton(tr("Delete Group"), this);
    m_deleteButton->setEnabled(false);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_newButton);
    buttonRow->addWidget(m_deleteButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_list);
    layout->addLayout(buttonRow);

    connect(m_newButton, &QPushButton::clicked, this, &GroupSidebar::onNewGroupClicked);
    connect(m_deleteButton, &QPushButton::clicked, this, &GroupSidebar::onDeleteGroupClicked);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &GroupSidebar::onSelectionChanged);
    connect(m_list, &QListWidget::itemClicked, this, &GroupSidebar::onItemClicked);

    refresh();
}

void GroupSidebar::refresh() {
    m_list->clear();

    QVector<Corpus> corpora;
    if (!m_engine->listCorpora(&corpora)) {
        QMessageBox::warning(this, tr("LEXIS"), tr("Could not load groups: %1").arg(m_engine->lastError()));
        return;
    }

    for (const Corpus &corpus : corpora) {
        auto *item = new QListWidgetItem(corpus.displayName, m_list);
        item->setData(CorpusIdRole, corpus.id);
    }
}

void GroupSidebar::onNewGroupClicked() {
    bool ok = false;
    QString name =
        QInputDialog::getText(this, tr("New Group"), tr("Group name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }

    qint64 newId = 0;
    if (!m_engine->createCorpus(name, &newId)) {
        QMessageBox::warning(this, tr("LEXIS"), tr("Could not create group: %1").arg(m_engine->lastError()));
        return;
    }
    refresh();
}

void GroupSidebar::onDeleteGroupClicked() {
    QListWidgetItem *item = m_list->currentItem();
    if (item == nullptr) {
        return;
    }

    qint64 corpusId = item->data(CorpusIdRole).toLongLong();
    QString name = item->text();

    QMessageBox::StandardButton answer =
        QMessageBox::question(this, tr("Delete Group"),
                               tr("Delete \"%1\" and everything in it? This cannot be undone.").arg(name),
                               QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    if (!m_engine->deleteCorpus(corpusId)) {
        QMessageBox::warning(this, tr("LEXIS"), tr("Could not delete group: %1").arg(m_engine->lastError()));
        return;
    }
    refresh();
}

void GroupSidebar::onSelectionChanged() {
    m_deleteButton->setEnabled(m_list->currentItem() != nullptr);
}

void GroupSidebar::onItemClicked(QListWidgetItem *item) {
    emit groupSelected(item->data(CorpusIdRole).toLongLong());
}
