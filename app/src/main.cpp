#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStyleHints>

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
