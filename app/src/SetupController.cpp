#include "SetupController.h"

extern "C" {
#include "config.h"
#include "paths.h"
}

#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

#include <cstdlib>

namespace {
bool g_bundleMode = false;

// Same repo-derivation rule as scripts/download_model.sh: strip the
// quantization suffix to recover the model name unsloth's "-GGUF"
// repos are named after. Empty result = underivable filename.
QString unslothUrlFor(const QString &modelFileName) {
    static const QRegularExpression quantSuffix(
        QStringLiteral("-(UD-)?(I?Q[0-9][A-Za-z0-9_]*)\\.gguf$"));
    QString name = modelFileName;
    if (name.remove(quantSuffix) == modelFileName) {
        return QString();
    }
    return QStringLiteral("https://huggingface.co/unsloth/%1-GGUF/resolve/main/%2")
        .arg(name, modelFileName);
}

const char *kRerankerUrl =
    "https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/"
    "bge-small-en-v1.5-f16.gguf";
} // namespace

void SetupController::configure(bool bundleMode) {
    g_bundleMode = bundleMode;
}

SetupController::SetupController(QObject *parent) : QObject(parent) {
    if (!g_bundleMode) {
        return; // dev build: overlay never shows
    }

    // The files the config actually points at -- absolute paths in a
    // bundle (AppEnvironment generated the config before QML loaded).
    const char *configPath = lexis_paths_config_file();
    char *modelPath = config_load_model_path(configPath);
    char *rerankerPath = config_load_reranker_model_path(configPath);

    if (modelPath != nullptr && !QFileInfo::exists(QString::fromUtf8(modelPath))) {
        const QString target = QString::fromUtf8(modelPath);
        const QString url = unslothUrlFor(QFileInfo(target).fileName());
        if (!url.isEmpty()) {
            m_pending.append({target, url, QStringLiteral("chat model (about 5 GB)")});
        }
    }
    if (rerankerPath != nullptr && !QFileInfo::exists(QString::fromUtf8(rerankerPath))) {
        m_pending.append({QString::fromUtf8(rerankerPath), QString::fromUtf8(kRerankerUrl),
                          QStringLiteral("reranker model (67 MB)")});
    }
    free(modelPath);
    free(rerankerPath);

    m_totalCount = m_pending.size();
    m_required = m_totalCount > 0;
    if (m_required) {
        m_statusText = QStringLiteral("LEXIS needs its language models (about 5.1 GB, one time).");
    }
}

void SetupController::startDownload() {
    if (m_downloading || m_pending.isEmpty()) {
        return;
    }
    m_errorText.clear();
    m_downloading = true;
    emit stateChanged();
    startNext();
}

void SetupController::startNext() {
    if (m_pending.isEmpty()) {
        m_downloading = false;
        m_finished = true;
        m_required = false;
        m_statusText = QStringLiteral("Setup complete.");
        emit stateChanged();
        emit setupComplete();
        return;
    }

    const Download item = m_pending.first();
    QDir().mkpath(QFileInfo(item.targetPath).absolutePath());

    // Resume a previous partial download if a .part file survives.
    m_partFile.setFileName(item.targetPath + ".part");
    m_resumeOffset = m_partFile.exists() ? m_partFile.size() : 0;
    if (!m_partFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        failWith(QStringLiteral("Could not write to %1").arg(m_partFile.fileName()));
        return;
    }

    QNetworkRequest request{QUrl(item.url)};
    if (m_resumeOffset > 0) {
        request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(m_resumeOffset) +
                                          "-");
    }
    const int fileNumber = m_totalCount - m_pending.size() + 1;
    m_statusText = QStringLiteral("Downloading the %1 (%2 of %3)...")
                       .arg(item.label)
                       .arg(fileNumber)
                       .arg(m_totalCount);
    m_progress = 0.0;
    emit stateChanged();
    emit progressChanged();

    m_reply = m_network.get(request);

    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        // A server that ignored our Range request restarts the file
        // from byte zero -- drop what we had so the file can't end up
        // doubled.
        if (m_resumeOffset > 0 &&
            m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            m_partFile.resize(0);
            m_resumeOffset = 0;
        }
        m_partFile.write(m_reply->readAll());
    });

    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total > 0) {
                    m_progress = double(m_resumeOffset + received) / double(m_resumeOffset + total);
                    emit progressChanged();
                }
            });

    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        m_partFile.close();
        const bool ok = (m_reply->error() == QNetworkReply::NoError);
        const QString errorString = m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;

        if (!ok) {
            // The .part file stays for a resume on the next attempt.
            failWith(QStringLiteral("Download failed: %1. Check your connection and try again "
                                    "-- it resumes where it stopped.")
                         .arg(errorString));
            return;
        }
        const Download item = m_pending.takeFirst();
        QFile::remove(item.targetPath);
        if (!QFile::rename(item.targetPath + ".part", item.targetPath)) {
            failWith(QStringLiteral("Could not move the finished download into place at %1")
                         .arg(item.targetPath));
            return;
        }
        startNext();
    });
}

void SetupController::failWith(const QString &message) {
    if (m_partFile.isOpen()) {
        m_partFile.close();
    }
    m_downloading = false;
    m_errorText = message;
    emit stateChanged();
}
