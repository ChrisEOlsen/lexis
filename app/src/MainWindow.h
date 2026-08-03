// Top-level application window. Deliberately bare at this stage -- this
// is the build/link scaffolding milestone (does the Qt app actually
// build, link against lexis_core, and run), not the group-management UI
// itself, which comes next. See ../../APP_SPEC.md.

#ifndef LEXIS_APP_MAINWINDOW_H
#define LEXIS_APP_MAINWINDOW_H

#include <QMainWindow>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
};

#endif // LEXIS_APP_MAINWINDOW_H
