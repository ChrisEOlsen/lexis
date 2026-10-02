// First-run model download for the bundle (dev builds never need it; see configure()).
// Fetches the two GGUFs into Application Support with progress and resume.

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

    // Whether the setup overlay must show (bundle mode + model file missing).
    Q_PROPERTY(bool required READ isRequired NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ isDownloading NOTIFY stateChanged)
    Q_PROPERTY(bool finished READ isFinished NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged) // 0..1 for the current file

public:
    explicit SetupController(QObject *parent = nullptr);

    // Called from main() before QML loads; dev builds never call it.
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
    // All models in place; Main.qml asks AppController to load the model now.
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
