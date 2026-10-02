// Line-preserving read/modify/write of config/lexis.conf for Settings: rewrites only the
// lines it owns, passing everything else through byte-for-byte (writes are atomic).

#ifndef LEXIS_APP_CONFIGMANAGER_H
#define LEXIS_APP_CONFIGMANAGER_H

#include <QString>
#include <QStringList>

class ConfigManager {
public:
    explicit ConfigManager(const QString &configPath);

    // Current on-disk state (thinking defaults to on when the line is missing).
    bool thinkingEnabled() const;
    bool rerankerEnabled() const;

    bool setThinkingEnabled(bool enabled);
    bool setRerankerEnabled(bool enabled);

    // The chat model's path (for read-only display in Settings).
    QString modelPath() const;

    QString lastError() const { return m_lastError; }

private:
    // Replaces the last `key = value` line (keeping any trailing comment), or appends
    // it when absent. False when the file can't be read or written.
    bool updateKeyLine(const QString &key, const QString &value);

    // Comments out EVERY active `key = value` line (a leftover duplicate would stay on
    // in the engine). No-op success when already commented out or absent.
    bool commentOutKeyLine(const QString &key);

    // Writes `lines` atomically (QSaveFile: temp file + rename).
    bool writeLines(const QStringList &lines);

    QString m_configPath;
    mutable QString m_lastError;
};

#endif // LEXIS_APP_CONFIGMANAGER_H