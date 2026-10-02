#include "AppEnvironment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <pwd.h>
#include <unistd.h>

namespace {
// From the password database, not $USER: peer auth compares against the real uid.
QString osUserName() {
    const struct passwd *pw = getpwuid(getuid());
    return pw != nullptr ? QString::fromUtf8(pw->pw_name) : QString();
}
} // namespace

AppEnvironment AppEnvironment::detect() {
    AppEnvironment env;

    // Bundle marker: the shipped wordnet payload (checking data, not just a directory
    // name, so a dev binary in a folder named MacOS can't misdetect).
    const QString resources =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../Resources");
    env.bundleMode = QFileInfo::exists(resources + "/data/wordnet/index.noun");

    if (!env.bundleMode) {
        // Dev tree: historical relative paths, untouched.
        env.configFile = QStringLiteral("config/lexis.conf");
        return env;
    }

    env.resourcesDir = resources;
    env.supportDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    env.configFile = env.supportDir + "/lexis.conf";
    env.modelsDir = env.supportDir + "/models";
    env.pgDataDir = env.supportDir + "/pgdata";
    env.pgSocketDir = env.supportDir + "/run";
    env.pgBinDir = env.resourcesDir + "/pgsql/bin";
    return env;
}

QString AppEnvironment::socketConninfo() const {
    // Unix socket + peer auth (the OS user is the credential; no password). Quoted
    // because the path contains a space and libpq splits on unquoted whitespace.
    return QStringLiteral("host='%1' dbname=lexis user=%2").arg(pgSocketDir, osUserName());
}

bool AppEnvironment::ensureSupportLayout(QString *error) const {
    if (!bundleMode) {
        return true;
    }

    for (const QString &dir : {supportDir, modelsDir, pgSocketDir}) {
        if (!QDir().mkpath(dir)) {
            if (error != nullptr) {
                *error = QStringLiteral("could not create %1").arg(dir);
            }
            return false;
        }
    }
    // Socket directory visible to this user only.
    QFile::setPermissions(pgSocketDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ExeOwner);

    if (QFileInfo::exists(configFile)) {
        return true;
    }

    // First run: shipped defaults with absolute Application Support paths
    // (thinking=off, reranker on: the measured-best configuration).
    QFile conf(configFile);
    if (!conf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not write %1").arg(configFile);
        }
        return false;
    }
    const QString contents =
        QStringLiteral("# LEXIS configuration -- generated on first run.\n"
                       "model_path=%1/gemma-4-E4B-it-Q4_K_M.gguf\n"
                       "reranker_model_path=%1/bge-small-en-v1.5-f16.gguf\n"
                       "thinking=off\n"
                       "# Private bundled Postgres over a unix socket; the OS user is\n"
                       "# the credential (peer auth), so there is no password.\n"
                       "db_conninfo=%2\n")
            .arg(modelsDir, socketConninfo());
    conf.write(contents.toUtf8());
    conf.close();
    return true;
}
