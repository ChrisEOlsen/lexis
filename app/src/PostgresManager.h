// Bundled Postgres child process: initdb on first run, start on launch, stop on quit.
// Bundle mode only; construct before the QML engine so connections close first.

#ifndef LEXIS_APP_POSTGRESMANAGER_H
#define LEXIS_APP_POSTGRESMANAGER_H

#include <QString>

class PostgresManager {
public:
    PostgresManager() = default;
    ~PostgresManager();

    // binDir: the bundled pgsql/bin (initdb, pg_ctl, createdb, ...).
    void configure(const QString &binDir, const QString &dataDir, const QString &socketDir);

    // Initdb/start/create-lexis-db as needed; a leftover server counts as started.
    bool ensureStarted(QString *error);

    void stop();

private:
    // Runs one bundled binary synchronously; returns its exit code,
    // captures combined output for error reporting.
    int run(const QString &program, const QStringList &args, QString *output) const;

    QString m_binDir;
    QString m_dataDir;
    QString m_socketDir;
    bool m_started = false;
};

#endif // LEXIS_APP_POSTGRESMANAGER_H
