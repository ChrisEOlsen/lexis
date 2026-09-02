// Line-preserving read/modify/write of config/lexis.conf for the
// Settings panel. The file is hand-readable and hand-editable by design
// (see docs/configuration.md) -- comments and unknown keys belong to
// whoever wrote them -- so a settings change rewrites ONLY the lines it
// owns and passes everything else through byte-for-byte:
//
//   - `thinking = on|off`: an existing line is replaced in place (its
//     comment, if any, is kept by appending the comment to the new
//     value's line -- see updateKeyLine()); a missing line is appended
//     at the end.
//   - `reranker_model_path`: enabled keeps an existing line or restores
//     a commented-out one; disabled comments the line out
//     (`# reranker_model_path=...`) so the remembered path survives for
//     the next enable instead of being destroyed. Enabling when the file
//     names no path at all FAILS rather than inventing one -- there is
//     no model to point at, and silently succeeding would show the
//     switch on while the engine has nothing to load.
//
// Failures are reported via the return values, never silent: a failed
// write leaves the file untouched (writes go through a .part file +
// rename, same discipline SetupController uses for model downloads).

#ifndef LEXIS_APP_CONFIGMANAGER_H
#define LEXIS_APP_CONFIGMANAGER_H

#include <QString>
#include <QStringList>

class ConfigManager {
public:
    explicit ConfigManager(const QString &configPath);

    // Current on-disk state. thinking defaults to on when the line is
    // missing (config_load_thinking()'s documented default); reranker
    // enabled means an uncommented reranker_model_path line exists.
    bool thinkingEnabled() const;
    bool rerankerEnabled() const;

    bool setThinkingEnabled(bool enabled);
    bool setRerankerEnabled(bool enabled);

    // The chat model's path (for read-only display in Settings).
    QString modelPath() const;

    QString lastError() const { return m_lastError; }

private:
    // Replaces the last `key = value` line's value (keeping any
    // trailing comment), or appends `key = value` when no such line
    // exists. Returns false when the file can't be read or written.
    bool updateKeyLine(const QString &key, const QString &value);

    // Comments out EVERY active `key = value` line (a "# " prefix); a
    // no-op success when they are all already commented out or the key
    // is absent (there is nothing to remember in those cases). All of
    // them, not just the first: the engine reads the last occurrence, so
    // leaving a duplicate behind would disable the setting in the panel
    // and leave it on in the engine.
    bool commentOutKeyLine(const QString &key);

    // Writes `lines` atomically (QSaveFile: temp file + rename, so a
    // failed write never leaves a half-written config).
    bool writeLines(const QStringList &lines);

    QString m_configPath;
    mutable QString m_lastError;
};

#endif // LEXIS_APP_CONFIGMANAGER_H