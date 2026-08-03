#include "MainWindow.h"

#include <QLabel>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("LEXIS"));
    resize(900, 600);

    auto *placeholder = new QLabel(tr("LEXIS"), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);
}
