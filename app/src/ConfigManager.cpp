#include "ConfigManager.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>

namespace {
// Matches "key = value" and "key=value" with optional leading whitespace,
// allowing a trailing comment ("# ..."). Capture 1 = the key, capture 2 =
// the value, capture 3 = the trailing comment when present.
//
// Applied to ONE line at a time, never to whole file contents:
// QRegularExpression has no MultilineOption by default, so against a
// multi-line subject "^" only matches at the very start of the string
// and a key line anywhere below the first would never match.
QRegularExpression keyLinePattern(const QString &key) {
    // The key is inserted raw into the pattern -- it is a fixed,
    // code-owned literal ("thinking", "reranker_model_path"), never user
    // input, so no escaping is needed.
    return QRegularExpression(
        QStringLiteral("^\\s*(%1)\\s*=\\s*([^#\\n]*?)\\s*(#.*)?$").arg(QRegularExpression::escape(key)));
}

QRegularExpression commentedKeyLinePattern(const QString &key) {
    return QRegularExpression(QStringLiteral("^\\s*#\\s*(%1)\\s*=").arg(QRegularExpression::escape(key)));
}

// Reads the file into lines, or reports failure through `ok`. Every
// reader and writer below goes through this, so they all see the file
// the same way.
QStringList readConfigLines(const QString &path, bool *ok) {
    QStringList lines;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *ok = false;
        return lines;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        lines.append(in.readLine());
    }
    *ok = true;
    return lines;
}

// Index of the LAST active (non-commented) line for `key`, or -1 when
// the key is absent. Last-one-wins on purpose: that is what the engine's
// own parser does (src/core/config.c's find_last_value), so a file with
// a duplicated key reads the same here as it does there. `outValue`, when
// given, receives that line's value with surrounding whitespace removed.
int lastKeyLineIndex(const QStringList &lines, const QString &key, QString *outValue) {
    const QRegularExpression pattern = keyLinePattern(key);
    int found = -1;
    for (int i = 0; i < lines.size(); i++) {
        const QRegularExpressionMatch match = pattern.match(lines.at(i));
        if (!match.hasMatch()) {
            continue;
        }
        found = i;
        if (outValue != nullptr) {
            *outValue = match.captured(2).trimmed();
        }
    }
    return found;
}
} // namespace

ConfigManager::ConfigManager(const QString &configPath) : m_configPath(configPath) {
}

bool ConfigManager::thinkingEnabled() const {
    bool ok = false;
    const QStringList lines = readConfigLines(m_configPath, &ok);
    if (!ok) {
        return true; // missing file = config's documented default (on)
    }
    QString value;
    if (lastKeyLineIndex(lines, QStringLiteral("thinking"), &value) < 0) {
        return true; // missing line = on, same default
    }
    // Only the literal "off" turns it off, matching config_load_thinking().
    return value.toLower() != QStringLiteral("off");
}

bool ConfigManager::rerankerEnabled() const {
    bool ok = false;
    const QStringList lines = readConfigLines(m_configPath, &ok);
    if (!ok) {
        return false; // missing file = no reranker line = off
    }
    QString value;
    if (lastKeyLineIndex(lines, QStringLiteral("reranker_model_path"), &value) < 0) {
        return false;
    }
    // An empty path is off, not on: config_load_reranker_model_path()
    // returns NULL for it, so the model would never load.
    return !value.isEmpty();
}

QString ConfigManager::modelPath() const {
    bool ok = false;
    const QStringList lines = readConfigLines(m_configPath, &ok);
    if (!ok) {
        return QString();
    }
    // model_path vs reranker_model_path: the anchored pattern's key
    // group makes this exact -- "reranker_model_path" never matches the
    // "model_path" pattern because the regex requires the whole key.
    QString value;
    if (lastKeyLineIndex(lines, QStringLiteral("model_path"), &value) < 0) {
        return QString();
    }
    return value;
}

bool ConfigManager::setThinkingEnabled(bool enabled) {
    return updateKeyLine(QStringLiteral("thinking"), enabled ? QStringLiteral("on") : QStringLiteral("off"));
}

bool ConfigManager::setRerankerEnabled(bool enabled) {
    if (enabled) {
        // Enabling: a commented-out line gets un-commented (restoring
        // the remembered path); otherwise nothing to do -- the shipped
        // config already carries an active line, and inventing a path
        // here would be a guess.
        bool ok = false;
        QStringList lines = readConfigLines(m_configPath, &ok);
        if (!ok) {
            m_lastError = QStringLiteral("Could not read %1").arg(m_configPath);
            return false;
        }

        // The LAST commented-out line, to pair with commentOutKeyLine()
        // below and with the readers' last-one-wins rule.
        const QRegularExpression commented = commentedKeyLinePattern(QStringLiteral("reranker_model_path"));
        int found = -1;
        for (int i = 0; i < lines.size(); i++) {
            if (commented.match(lines.at(i)).hasMatch()) {
                found = i;
            }
        }
        if (found >= 0) {
            // Strip the leading '#' and whitespace, keep the rest
            // ("reranker_model_path=data/models/bge-small...gguf").
            lines[found] = lines.at(found).mid(lines.at(found).indexOf(QLatin1Char('#')) + 1).trimmed();
            return writeLines(lines);
        }
        if (rerankerEnabled()) {
            return true; // already active: nothing to write
        }
        // Neither an active line nor a commented-out one: there is no
        // path to enable. Reporting success here would light the switch
        // up while config_load_reranker_model_path() still returns NULL,
        // so the model never loads and the next launch shows it off again.
        m_lastError = QStringLiteral("no reranker_model_path in %1 -- add one to use reranking").arg(m_configPath);
        return false;
    }
    return commentOutKeyLine(QStringLiteral("reranker_model_path"));
}

bool ConfigManager::updateKeyLine(const QString &key, const QString &value) {
    bool ok = false;
    QStringList lines = readConfigLines(m_configPath, &ok);
    if (!ok) {
        m_lastError = QStringLiteral("Could not read %1").arg(m_configPath);
        return false;
    }

    // The line the readers (and the engine) would take: the last one.
    // Rewriting an earlier duplicate instead would save a value nothing
    // ever reads back.
    const int index = lastKeyLineIndex(lines, key, nullptr);
    if (index >= 0) {
        const QString comment = keyLinePattern(key).match(lines.at(index)).captured(3);
        lines[index] = QStringLiteral("%1=%2").arg(key, value);
        if (!comment.isEmpty()) {
            lines[index] += QStringLiteral("  ") + comment;
        }
        return writeLines(lines);
    }
    lines.append(QStringLiteral("%1=%2").arg(key, value));
    return writeLines(lines);
}

bool ConfigManager::commentOutKeyLine(const QString &key) {
    bool ok = false;
    QStringList lines = readConfigLines(m_configPath, &ok);
    if (!ok) {
        m_lastError = QStringLiteral("Could not read %1").arg(m_configPath);
        return false;
    }

    // Every active line, not just the first: leaving a duplicate behind
    // would mean "off" in the panel and still-on in the engine.
    const QRegularExpression active = keyLinePattern(key);
    bool changed = false;
    for (int i = 0; i < lines.size(); i++) {
        if (active.match(lines.at(i)).hasMatch()) {
            lines[i] = QStringLiteral("# ") + lines.at(i);
            changed = true;
        }
    }
    if (!changed) {
        return true; // already commented out or absent: nothing to do
    }
    return writeLines(lines);
}

bool ConfigManager::writeLines(const QStringList &lines) {
    QSaveFile file(m_configPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Could not write %1").arg(m_configPath);
        return false;
    }
    QTextStream out(&file);
    for (const QString &line : lines) {
        out << line << "\n";
    }
    if (!file.commit()) {
        m_lastError = QStringLiteral("Could not save %1").arg(m_configPath);
        return false;
    }
    return true;
}