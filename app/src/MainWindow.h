// Top-level application window: owns the engine connection and lays out
// the group sidebar against a (currently placeholder) main content
// area. See ../../APP_SPEC.md.

#ifndef LEXIS_APP_MAINWINDOW_H
#define LEXIS_APP_MAINWINDOW_H

#include <QMainWindow>

#include <memory>

class LexisEngine;
class GroupSidebar;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onGroupSelected(qint64 corpusId);

private:
    std::unique_ptr<LexisEngine> m_engine;
    GroupSidebar *m_sidebar;
    QLabel *m_content; // placeholder for the document/search view to come
};

#endif // LEXIS_APP_MAINWINDOW_H
