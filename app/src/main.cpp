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
    // Deliberately NOT forcing QSG_RENDER_LOOP=basic. That was added as
    // a diagnostic while chasing hover/animation flicker and a popup
    // that painted nothing; both turned out to be QML-side bugs (a
    // Drawer with no explicit height collapsing to 26px, and three
    // interaction states sharing one nearly-invisible color token), not
    // a render-loop problem. Forcing the single-threaded loop is a
    // debugging aid, not a supported production configuration on macOS,
    // where the threaded loop is the default and the tested path -- on
    // the basic loop animations are advanced from the same thread that
    // does the drawing, so any main-thread work stalls them mid-flight,
    // which produces exactly the stutter it was meant to diagnose.
    QGuiApplication app(argc, argv);
    // Names ~/Library/Application Support/LEXIS (via QStandardPaths) --
    // must be set before AppEnvironment::detect() asks for it. No
    // organization name on purpose: QStandardPaths on macOS appends
    // BOTH names, which would nest the directory as LEXIS/LEXIS.
    QCoreApplication::setApplicationName("LEXIS");

    // Installed .app bundle vs. dev build tree. In a bundle: point the
    // C core at Resources/ (read-only data) and Application Support
    // (config, models), then run the private bundled Postgres as a
    // child process. In a dev tree every branch below is skipped and
    // behavior is exactly what it always was.
    //
    // pgManager is a stack object declared BEFORE the QML engine on
    // purpose: C++ destroys in reverse order, so the engine (and with
    // it AppController's open database connections) goes down first,
    // then pgManager's destructor stops the server.
    const AppEnvironment env = AppEnvironment::detect();
    PostgresManager pgManager;
    QString envError;
    if (env.bundleMode) {
        lexis_paths_set(env.resourcesDir.toUtf8().constData(), env.configFile.toUtf8().constData());
        // OcrExtractor reads this instead of the compile-time Homebrew
        // tessdata path.
        qputenv("LEXIS_TESSDATA_DIR", (env.resourcesDir + "/tessdata").toUtf8());
        if (env.ensureSupportLayout(&envError)) {
            pgManager.configure(env.pgBinDir, env.pgDataDir, env.pgSocketDir);
            pgManager.ensureStarted(&envError);
        }
        if (!envError.isEmpty()) {
            // Continue anyway: AppController's own connection check will
            // surface a dialog, and this detail lands in the log.
            qCritical("LEXIS startup: %s", qUtf8Printable(envError));
        }
    }
    SetupController::configure(env.bundleMode);

    // FluentWinUI3 rather than Basic. Basic supplies no design language
    // of its own -- it is a customization substrate -- so every control
    // needed a hand-written background/contentItem, and everything the
    // app did not restyle (Menu, ComboBox, ScrollBar, ToolTip, focus
    // rings, keyboard navigation) stayed unstyled. FluentWinUI3 ships 49
    // styled controls with light and dark image assets, so qml/controls/
    // is gone entirely and Theme.qml no longer carries color.
    //
    // Despite the name, this style is not Windows-only. Qt's docs:
    // "This style is not native and can be run on all supported
    // platforms with minor differences due to system fonts and rendering
    // engines." Available since Qt 6.8; this build is 6.11.1. Verified
    // present in this install at
    // /opt/homebrew/share/qt/qml/QtQuick/Controls/FluentWinUI3.
    //
    // The native macOS style was considered and rejected: it refuses
    // background/contentItem overrides outright (confirmed via runtime
    // warning, "The current style does not support customization of this
    // control"), which this app still needs in the few places where the
    // stock contentItem cannot elide long text.
    //
    // Must be set before the QML engine loads anything importing
    // QtQuick.Controls.
    QQuickStyle::setStyle("FluentWinUI3");

    // Pin dark. FluentWinUI3 follows the system palette on its own, so
    // dropping this one line is all that a follow-the-OS (or light) app
    // would take -- there is no second set of tokens to maintain, which
    // is the point of having the style own color rather than Theme.qml.
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Lexis", "Main");

    return app.exec();
}
