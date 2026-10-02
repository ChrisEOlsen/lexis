#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStyleHints>

#include "AppEnvironment.h"
#include "PostgresManager.h"
#include "SetupController.h"

extern "C" {
#include "paths.h"
}

int main(int argc, char *argv[]) {
    // No QSG_RENDER_LOOP override: the flicker it once diagnosed was QML-side bugs,
    // and the single-threaded loop stalls animations on main-thread work.
    QGuiApplication app(argc, argv);
    // Set before AppEnvironment::detect(); no org name (macOS would nest LEXIS/LEXIS).
    QCoreApplication::setApplicationName("LEXIS");

    // Bundle mode: point the C core at Resources/Application Support, start bundled Postgres.
    // pgManager is declared before the engine so connections close before the server stops.
    const AppEnvironment env = AppEnvironment::detect();
    PostgresManager pgManager;
    QString envError;
    if (env.bundleMode) {
        lexis_paths_set(env.resourcesDir.toUtf8().constData(), env.configFile.toUtf8().constData());
        // Overrides OcrExtractor's compile-time Homebrew tessdata path.
        qputenv("LEXIS_TESSDATA_DIR", (env.resourcesDir + "/tessdata").toUtf8());
        if (env.ensureSupportLayout(&envError)) {
            pgManager.configure(env.pgBinDir, env.pgDataDir, env.pgSocketDir);
            pgManager.ensureStarted(&envError);
        }
        if (!envError.isEmpty()) {
            // Continue anyway: AppController surfaces a dialog; this lands in the log.
            qCritical("LEXIS startup: %s", qUtf8Printable(envError));
        }
    }
    SetupController::configure(env.bundleMode);

    // FluentWinUI3 (not Basic, which styles nothing, nor macOS native, which forbids
    // contentItem overrides). Must be set before the QML engine loads. Works on macOS.
    QQuickStyle::setStyle("FluentWinUI3");

    // Pin dark; dropping this line follows the OS (no second token set to maintain).
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Lexis", "Main");

    return app.exec();
}
