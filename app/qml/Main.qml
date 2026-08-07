// Root window. Lays out the group sidebar against the chat panel and
// shows AppController's notify() signal as a dismissible dialog -- the
// one place both children funnel user-facing messages through.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

ApplicationWindow {
    id: window
    visible: true
    width: 1300
    height: 760
    minimumWidth: 900
    minimumHeight: 560
    title: "LEXIS"

    // No explicit `color`. ApplicationWindow is one of the controls
    // FluentWinUI3 implements, so the window background is the style's --
    // which is what makes it match the dialogs and popups drawn over it
    // without a shared color token to keep in sync.

    Connections {
        target: AppController
        function onNotify(message) {
            messageDialog.text = message
            messageDialog.open()
        }
    }

    Dialog {
        id: messageDialog
        title: "LEXIS"
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Ok
        property alias text: messageLabel.text

        // Explicit width, and an explicit width on the wrapping Label
        // below. Without both this is a real binding loop, reported at
        // runtime as 'QML Dialog: Binding loop detected for property
        // "implicitWidth"': Dialog.implicitWidth derives from its
        // content's implicitWidth, a WordWrap Label's implicitWidth
        // derives from the width it was given, and that width comes from
        // the Dialog. A loop re-runs layout every frame, visible as the
        // dialog jittering as it opens.
        width: Math.min(440, window.width - 2 * Theme.spacingXL)

        Label {
            id: messageLabel
            width: messageDialog.availableWidth
            wrapMode: Text.WordWrap
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingM
        spacing: Theme.spacingM

        GroupSidebar {
            Layout.preferredWidth: 260
            Layout.fillHeight: true
        }

        ChatPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
