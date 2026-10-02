// Layout tokens only, deliberately no color: the style (FluentWinUI3) owns color and
// surfaces derive from the live `palette`, so a light mode is a one-line change.
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

    // -- Surface elevation (Qt.lighter() multipliers; Fluent layers fills, not shadows) --
    readonly property real layerCard: 1.25   // sidebar / panel card
    readonly property real layerRaised: 1.35 // bubble, chip, popup content

    // -- Typography (only text this app draws itself; controls keep the style's sizing) --
    readonly property int fontSizeTitle: 20
    readonly property int fontSizeCaption: 12
    // Assistant answers only -- a notch above the control default so the
    // content users actually read leads the visual hierarchy.
    readonly property int fontSizeAnswer: 15
    readonly property int fontWeightBold: Font.DemiBold

    // -- Motion (one nav duration; the style owns all control feedback) --
    readonly property int durationNav: 160
}
