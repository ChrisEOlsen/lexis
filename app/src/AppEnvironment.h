// Decides, once at startup, whether the app is running out of an
// installed .app bundle or straight from a development build tree --
// and resolves every path that differs between the two.
//
// Dev build (cmake build dir, working directory = repo root): nothing
// changes. All paths stay the historical working-directory-relative
// ones ("config/lexis.conf", "data/wordnet", the Homebrew tessdata),
// so `make check`, the CLI, and the dev app keep behaving exactly as
// before this class existed.
//
// Bundle (LEXIS.app): read-only data ships inside
// Contents/Resources/ and mutable state lives in
// ~/Library/Application Support/LEXIS/ (config, models, the private
// Postgres data directory, its unix socket). detect() finds the
// Resources dir by looking for the wordnet data next to the binary;
// main.cpp then hands both roots to the C core via lexis_paths_set()
// before anything lazily loads a path.

#ifndef LEXIS_APP_APPENVIRONMENT_H
#define LEXIS_APP_APPENVIRONMENT_H

#include <QString>

class AppEnvironment {
public:
    // Call after QGuiApplication exists (needs applicationDirPath()).
    static AppEnvironment detect();

    // Creates the Application Support directories (support root, models,
    // run/ at mode 0700 for the Postgres socket) and writes the default
    // lexis.conf if none exists yet. Bundle mode only; no-op otherwise.
    // Returns false with *error set if a directory or the config file
    // could not be created.
    bool ensureSupportLayout(QString *error) const;

    bool bundleMode = false;
    QString resourcesDir; // .app Contents/Resources (bundle mode only)
    QString supportDir;   // ~/Library/Application Support/LEXIS
    QString configFile;   // supportDir/lexis.conf (bundle) or config/lexis.conf (dev)
    QString modelsDir;    // supportDir/models
    QString pgDataDir;    // supportDir/pgdata
    QString pgSocketDir;  // supportDir/run -- 0700, socket-only Postgres
    QString pgBinDir;     // resourcesDir/pgsql/bin (bundle mode only)

    // The conninfo the generated config uses: unix socket + peer auth,
    // so no password exists anywhere (dev/PACKAGE.md's "real fix").
    QString socketConninfo() const;
};

#endif // LEXIS_APP_APPENVIRONMENT_H
