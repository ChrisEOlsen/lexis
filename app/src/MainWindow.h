// Top-level application window: owns the engine connection, the
// language data every ingest needs (loaded once, reused for the app's
// whole lifetime -- see IngestWorker's own comment on why), and lays
// out the group sidebar against the content view. See ../../APP_SPEC.md.

#ifndef LEXIS_APP_MAINWINDOW_H
#define LEXIS_APP_MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>

#include <memory>

extern "C" {
#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"
}

class LexisEngine;
class GroupSidebar;
class GroupContentView;
class IngestWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onGroupSelected(qint64 corpusId);
    void onFilesDropped(QStringList localPaths);
    void onIngestFinished(bool ok, qint64 totalPassages);

private:
    std::unique_ptr<LexisEngine> m_engine;
    GroupSidebar *m_sidebar;
    GroupContentView *m_content;
    IngestWorker *m_activeWorker; // non-owning; deletes itself via QThread::finished -> deleteLater()

    qint64 m_activeCorpusId;

    // Loaded once in the constructor, freed in the destructor -- shared
    // read-only across every IngestWorker rather than reloaded per drop.
    StopwordSet *m_stopwords;
    WordNetTable *m_wordnet;
    Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_MAINWINDOW_H
