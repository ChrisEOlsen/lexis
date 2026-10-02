// Decides at startup whether this is an installed .app bundle or a dev build, and
// resolves every path that differs (Resources vs. working-directory-relative).

#ifndef LEXIS_APP_APPENVIRONMENT_H
#define LEXIS_APP_APPENVIRONMENT_H

#include <QString>

class AppEnvironment {
public:
    // Call after QGuiApplication exists (needs applicationDirPath()). Bundle detection
    // looks for the shipped wordnet data next to the binary.
    static AppEnvironment detect();

    // Creates the Application Support layout and default lexis.conf if missing.
    // Bundle mode only; no-op otherwise. False with *error set on failure.
    bool ensureSupportLayout(QString *error) const;

    bool bundleMode = false;
    QString resourcesDir; // .app Contents/Resources (bundle mode only)
    QString supportDir;   // ~/Library/Application Support/LEXIS
    QString configFile;   // supportDir/lexis.conf (bundle) or config/lexis.conf (dev)
    QString modelsDir;    // supportDir/models
    QString pgDataDir;    // supportDir/pgdata
    QString pgSocketDir;  // supportDir/run -- 0700, socket-only Postgres
    QString pgBinDir;     // resourcesDir/pgsql/bin (bundle mode only)

    // Conninfo for the generated config: unix socket + peer auth, no password.
    QString socketConninfo() const;
};

#endif // LEXIS_APP_APPENVIRONMENT_H
