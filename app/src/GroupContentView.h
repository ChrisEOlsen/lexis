// Main content area: shows the active group's documents and accepts
// drag-and-drop file drops to add more. A "dumb" view -- ingestion is
// long-running and needs the worker-thread treatment MainWindow owns
// (see IngestWorker), so this class doesn't call into it directly; it
// just shows state and emits filesDropped() for MainWindow to act on.

#ifndef LEXIS_APP_GROUPCONTENTVIEW_H
#define LEXIS_APP_GROUPCONTENTVIEW_H

#include <QStringList>
#include <QWidget>

class LexisEngine;
class QLabel;
class QListWidget;

class GroupContentView : public QWidget {
    Q_OBJECT

public:
    explicit GroupContentView(LexisEngine *engine, QWidget *parent = nullptr);

    // Call only after LexisEngine::useCorpus() has already scoped the
    // connection to this group -- refresh() reads through that same
    // scoped connection, it doesn't take a corpus id of its own.
    void setActiveGroup(qint64 corpusId, const QString &displayName);
    void clearActiveGroup();
    void refresh();

    // Disables the whole view (including drops) while true -- also
    // what prevents a second drop from starting a second concurrent
    // rebuild while one is already running.
    void setBusy(bool busy, const QString &statusText = QString());

signals:
    void filesDropped(QStringList localPaths);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    LexisEngine *m_engine; // not owned
    qint64 m_activeCorpusId;
    QLabel *m_titleLabel;
    QLabel *m_statusLabel;
    QListWidget *m_documentList;
};

#endif // LEXIS_APP_GROUPCONTENTVIEW_H
