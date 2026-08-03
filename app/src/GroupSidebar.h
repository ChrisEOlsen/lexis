// Left-hand "groups" panel: lists every corpus via LexisEngine, lets
// the user create/delete groups and pick which one is active. Doesn't
// own LexisEngine's lifetime (MainWindow does) -- just calls into it.

#ifndef LEXIS_APP_GROUPSIDEBAR_H
#define LEXIS_APP_GROUPSIDEBAR_H

#include <QWidget>

class LexisEngine;
class QListWidget;
class QListWidgetItem;
class QPushButton;

class GroupSidebar : public QWidget {
    Q_OBJECT

public:
    explicit GroupSidebar(LexisEngine *engine, QWidget *parent = nullptr);

    // Reloads the list from the engine -- called once at construction
    // and again after any create/delete.
    void refresh();

signals:
    void groupSelected(qint64 corpusId);

private slots:
    void onNewGroupClicked();
    void onDeleteGroupClicked();
    void onSelectionChanged();
    void onItemClicked(QListWidgetItem *item);

private:
    LexisEngine *m_engine; // not owned
    QListWidget *m_list;
    QPushButton *m_newButton;
    QPushButton *m_deleteButton;
};

#endif // LEXIS_APP_GROUPSIDEBAR_H
