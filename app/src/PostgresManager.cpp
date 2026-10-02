#include "PostgresManager.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

PostgresManager::~PostgresManager() {
    stop();
}

void PostgresManager::configure(const QString &binDir, const QString &dataDir,
                                const QString &socketDir) {
    m_binDir = binDir;
    m_dataDir = dataDir;
    m_socketDir = socketDir;
}

int PostgresManager::run(const QString &program, const QStringList &args, QString *output) const {
    QProcess proc;
    // Pin a locale: pg_ctl fails without one outside a login shell (seen with Homebrew).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "en_US.UTF-8");
    proc.setProcessEnvironment(env);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(m_binDir + "/" + program, args);
    if (!proc.waitForFinished(60000)) {
        if (output != nullptr) {
            *output = QStringLiteral("%1 timed out").arg(program);
        }
        proc.kill();
        return -1;
    }
    if (output != nullptr) {
        *output = QString::fromUtf8(proc.readAll());
    }
    return proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
}

bool PostgresManager::ensureStarted(QString *error) {
    QString out;

    if (!QFileInfo::exists(m_dataDir + "/PG_VERSION")) {
        // -A peer: the OS user is the credential (matches AppEnvironment's conninfo).
        if (run("initdb", {"-D", m_dataDir, "-A", "peer", "-E", "UTF8", "--locale=C"}, &out) != 0) {
            if (error != nullptr) {
                *error = QStringLiteral("initdb failed: %1").arg(out.right(500));
            }
            return false;
        }
    }

    // Force socket-only every launch (appended last, so these win over older policy).
    QFile conf(m_dataDir + "/postgresql.conf");
    if (conf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString marker = QStringLiteral("# LEXIS socket-only overrides");
        const bool present = QString::fromUtf8(conf.readAll()).contains(marker);
        conf.close();
        if (!present && conf.open(QIODevice::Append | QIODevice::Text)) {
            conf.write(QStringLiteral("\n%1\n"
                                      "listen_addresses = ''\n"
                                      "unix_socket_directories = '%2'\n")
                           .arg(marker, m_socketDir)
                           .toUtf8());
            conf.close();
        }
    }

    // Already running (slow quit or crashed run) counts as started: same data dir.
    if (run("pg_ctl", {"-D", m_dataDir, "status"}, nullptr) != 0) {
        if (run("pg_ctl",
                {"-D", m_dataDir, "-w", "-t", "30", "-l", m_dataDir + "/server.log", "start"},
                &out) != 0) {
            if (error != nullptr) {
                *error = QStringLiteral("could not start the database: %1").arg(out.right(500));
            }
            return false;
        }
    }
    m_started = true;

    // Create the app's database if this is the first run. "already
    // exists" is the normal case afterwards, not an error.
    if (run("createdb", {"-h", m_socketDir, "lexis"}, &out) != 0 &&
        !out.contains(QStringLiteral("already exists"))) {
        if (error != nullptr) {
            *error = QStringLiteral("could not create the lexis database: %1").arg(out.right(500));
        }
        return false;
    }
    return true;
}

void PostgresManager::stop() {
    if (!m_started) {
        return;
    }
    m_started = false;
    run("pg_ctl", {"-D", m_dataDir, "-w", "-t", "20", "-m", "fast", "stop"}, nullptr);
}
