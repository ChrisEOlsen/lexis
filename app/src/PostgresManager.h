// Runs the PostgreSQL server that ships inside the app bundle as a
// child process: initdb on first run, start on launch, stop when the
// app quits. Bundle mode only -- the dev build keeps using the native
// Homebrew instance via `make pg-start` and never constructs one of
// these.
//
// Security model (dev/PACKAGE.md): no TCP at all (listen_addresses =
// ''), a unix socket in a 0700 directory under Application Support,
// and peer auth -- the OS user is the credential, so no database
// password exists anywhere on the machine.
//
// Lifecycle: ensureStarted() is synchronous and runs before the QML
// engine loads (initdb takes ~1-2s once, pg_ctl start ~1s). The
// destructor stops the server -- construct this BEFORE the QML engine
// in main() so C++ destruction order closes the app's database
// connections first, then shuts the server down.

#ifndef LEXIS_APP_POSTGRESMANAGER_H
#define LEXIS_APP_POSTGRESMANAGER_H

#include <QString>

class PostgresManager {
public:
    PostgresManager() = default;
    ~PostgresManager();

    // binDir: the bundled pgsql/bin (initdb, pg_ctl, createdb, ...).
    void configure(const QString &binDir, const QString &dataDir, const QString &socketDir);

    // initdb if the data directory is empty, force socket-only
    // configuration, start the server (a leftover from a crashed
    // previous run counts as started), and create the `lexis` database
    // if missing. Returns false with *error set on the first failure.
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
