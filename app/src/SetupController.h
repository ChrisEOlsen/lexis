// First-run model download. The bundle ships everything except the two
// GGUF models (~5.1GB together -- far too big for a DMG); this object
// backs the QML setup overlay that fetches them into
// ~/Library/Application Support/LEXIS/models/ with progress and
// resume.
//
// A QML singleton like AppController. In a dev build (or once both
// model files exist) `required` is false and the overlay never shows.
// main.cpp calls configure() before the QML engine loads so the
// constructor knows whether it is in a bundle and where models go.
//
// Download sources: the chat model URL is derived from the config's
// model_path filename with the same unsloth-repo rule
// scripts/download_model.sh uses; the reranker comes from a fixed
// CompendiumLabs URL. Downloads write to <file>.part and rename on
// completion, resuming a partial .part with an HTTP Range request.

#ifndef LEXIS_APP_SETUPCONTROLLER_H
#define LEXIS_APP_SETUPCONTROLLER_H

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQmlEngine>
#include <QString>

class QNetworkReply;

class SetupController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Whether the setup overlay must be shown at all (bundle mode with
    // at least one model file missing at startup).
    Q_PROPERTY(bool required READ isRequired NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ isDownloading NOTIFY stateChanged)
    Q_PROPERTY(bool finished READ isFinished NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged) // 0..1 for the current file

public:
    explicit SetupController(QObject *parent = nullptr);

    // Called from main() before the QML engine instantiates this
    // singleton. Dev builds never call it, leaving `required` false.
    static void configure(bool bundleMode);

    bool isRequired() const { return m_required; }
    bool isDownloading() const { return m_downloading; }
    bool isFinished() const { return m_finished; }
    QString statusText() const { return m_statusText; }
    QString errorText() const { return m_errorText; }
    double progress() const { return m_progress; }

    Q_INVOKABLE void startDownload();

signals:
    void stateChanged();
    void progressChanged();
    // Emitted once every model file is in place -- Main.qml reacts by
    // asking AppController to load the model it skipped at startup.
    void setupComplete();

private:
    struct Download {
        QString targetPath; // final .gguf path
        QString url;
        QString label; // user-facing name
    };

    void startNext();
    void failWith(const QString &message);

    QNetworkAccessManager m_network;
    QList<Download> m_pending;
    int m_totalCount = 0;
    QNetworkReply *m_reply = nullptr;
    QFile m_partFile;
    qint64 m_resumeOffset = 0;

    bool m_required = false;
    bool m_downloading = false;
    bool m_finished = false;
    QString m_statusText;
    QString m_errorText;
    double m_progress = 0.0;
};

#endif // LEXIS_APP_SETUPCONTROLLER_H
