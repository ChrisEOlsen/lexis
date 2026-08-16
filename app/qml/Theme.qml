// Layout tokens for the whole app. Deliberately NO color tokens.
//
// Color is owned by the Qt Quick Controls style (FluentWinUI3, pinned in
// main.cpp), not by this file. Every surface below the control layer
// derives its color from the live `palette` instead -- palette.window,
// palette.accent, palette.midlight, and so on -- which is why plain
// Items can be styled here at all: QQuickItem exposes `palette` and it
// resolves through the item tree (verified at runtime: a bare Item
// reports palette.window #1e1e1e under this style).
//
// The practical consequence is that adding a light mode is a one-line
// change in main.cpp rather than a second set of tokens, and that no
// hand-picked hex value can drift out of step with the style's own
// controls sitting next to it. The previous version of this file was a
// full hand-built palette, which is exactly the maintenance surface that
// adopting a real style removes.
//
// What stays here is geometry and timing: values the style has no
// opinion about because they describe this app's layout, not its
// controls.
pragma Singleton
import QtQuick

QtObject {
    // -- Spacing scale --
    readonly property int spacingXS: 4
    readonly property int spacingS: 8
    readonly property int spacingM: 12
    readonly property int spacingL: 16
    readonly property int spacingXL: 24

    // -- Corner radius -- Fluent uses 4 for controls, 8 for cards and
    // dialogs, and a larger value for speech-bubble style surfaces.
    readonly property int radiusS: 4
    readonly property int radiusM: 8
    readonly property int radiusL: 12

    // -- Surface elevation --
    // Fluent builds depth by layering lighter fills over the window
    // color rather than by drawing shadows. These are the multipliers
    // for Qt.lighter() against palette.window, kept here so every
    // surface in the app steps by the same amount.
    readonly property real layerCard: 1.25   // sidebar / panel card
    readonly property real layerRaised: 1.35 // bubble, chip, popup content

    // -- Typography --
    // Only sizes for text this app draws itself (titles, captions, chat
    // bubbles). Controls are left to the style's own font sizing --
    // overriding that is what makes a styled app look subtly wrong.
    readonly property int fontSizeTitle: 20
    readonly property int fontSizeCaption: 12
    // Assistant answers only -- a notch above the control default so the
    // content users actually read leads the visual hierarchy.
    readonly property int fontSizeAnswer: 15
    readonly property int fontWeightBold: Font.DemiBold

    // -- Motion --
    // One duration for panel navigation. No state-change animations
    // anywhere: the style owns control feedback, and this app adds none
    // of its own on top.
    readonly property int durationNav: 160
}
