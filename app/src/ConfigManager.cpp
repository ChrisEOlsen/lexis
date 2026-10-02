#include "ConfigManager.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>

namespace {
// Matches one "key = value" line (captures: key, value, trailing comment). One line at
// a time only: without MultilineOption, "^" matches just the string's start.
QRegularExpression keyLinePattern(const QString &key) {
    // Keys are fixed code-owned literals, never user input (still escaped anyway).
    return QRegularExpression(
        QStringLiteral("^\\s*(%1)\\s*=\\s*([^#\\n]*?)\\s*(#.*)?$").arg(QRegularExpression::escape(key)));
}

QRegularExpression commentedKeyLinePattern(const QString &key) {
    return QRegularExpression(QStringLiteral("^\\s*#\\s*(%1)\\s*=").arg(QRegularExpression::escape(key)));
}

// Reads the file into lines; every reader/writer below goes through this.
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

// Index of the LAST active line for `key` (-1 when absent); last-one-wins matches the
// engine's own parser. `outValue` receives the trimmed value when non-null.
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
        return true; // missing file: default on
    }
    QString value;
    if (lastKeyLineIndex(lines, QStringLiteral("thinking"), &value) < 0) {
        return true; // missing line: default on
    }
    // Only the literal "off" turns it off, matching config_load_thinking().
    return value.toLower() != QStringLiteral("off");
}

bool ConfigManager::rerankerEnabled() const {
    bool ok = false;
    const QStringList lines = readConfigLines(m_configPath, &ok);
    if (!ok) {
        return false; // missing file: off
    }
    QString value;
    if (lastKeyLineIndex(lines, QStringLiteral("reranker_model_path"), &value) < 0) {
        return false;
    }
    // An empty path is off: the engine returns NULL for it and the model never loads.
    return !value.isEmpty();
}

QString ConfigManager::modelPath() const {
    bool ok = false;
    const QStringList lines = readConfigLines(m_configPath, &ok);
    if (!ok) {
        return QString();
    }
    // Exact key match: "reranker_model_path" never matches the "model_path" pattern.
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
        // Enabling un-comments the remembered path; it never invents one.
        bool ok = false;
        QStringList lines = readConfigLines(m_configPath, &ok);
        if (!ok) {
            m_lastError = QStringLiteral("Could not read %1").arg(m_configPath);
            return false;
        }

        // The LAST commented-out line, pairing with the last-one-wins rule.
        const QRegularExpression commented = commentedKeyLinePattern(QStringLiteral("reranker_model_path"));
        int found = -1;
        for (int i = 0; i < lines.size(); i++) {
            if (commented.match(lines.at(i)).hasMatch()) {
                found = i;
            }
        }
        if (found >= 0) {
            // Strip the leading '#' and whitespace, keep the rest.
            lines[found] = lines.at(found).mid(lines.at(found).indexOf(QLatin1Char('#')) + 1).trimmed();
            return writeLines(lines);
        }
        if (rerankerEnabled()) {
            return true; // already active: nothing to write
        }
        // No line at all means no path to enable: fail rather than light the switch
        // while the engine still has nothing to load.
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

    // Rewrite the last line: the one the readers and the engine actually take.
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

    // Every active line: a leftover duplicate would stay on in the engine.
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