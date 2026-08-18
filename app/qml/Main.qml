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

    // Sidebar collapse -- pure UI state, toggled from ChatPanel's header
    // button. Fully collapsible: the panel animates to zero width and
    // fades, leaving the whole window to the conversation. The animated
    // value is this plain property, not the Layout attached property --
    // Behavior on attached properties is not reliably supported.
    property bool sidebarCollapsed: false
    property real sidebarWidth: sidebarCollapsed ? 0 : 260
    Behavior on sidebarWidth {
        NumberAnimation { duration: Theme.durationNav; easing.type: Easing.OutCubic }
    }

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
            Layout.preferredWidth: window.sidebarWidth
            Layout.fillHeight: true
            // Fade with the width so mid-animation content reads as
            // sliding away rather than being crushed.
            opacity: window.sidebarWidth / 260
            // At width 0 the item (and its 1px border) must not paint,
            // and it must not eat the RowLayout's spacing either.
            visible: window.sidebarWidth > 0.5
            clip: true
        }

        ChatPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            onSidebarToggleRequested: window.sidebarCollapsed = !window.sidebarCollapsed
        }
    }

    // First-run setup: shown only in an installed bundle whose language
    // models haven't been downloaded yet (SetupController.required is
    // always false in a dev build). Covers the whole window until the
    // ~5.1GB download lands, then asks AppController to start the model
    // load it skipped at startup.
    Connections {
        target: SetupController
        function onSetupComplete() {
            AppController.retryModelLoad()
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: SetupController.required || SetupController.downloading
        // Opaque black, not a translucent scrim -- the UI underneath is
        // alive (animations, loading states) and movement bleeding
        // through the setup screen looks broken.
        color: "black"

        // Swallow clicks so the UI underneath is inert during setup.
        MouseArea {
            anchors.fill: parent
        }

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(480, window.width - 2 * Theme.spacingXL)
            spacing: Theme.spacingL

            Label {
                text: "Welcome to LEXIS"
                font.pixelSize: Theme.fontSizeTitle
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                text: SetupController.statusText
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }

            ProgressBar {
                visible: SetupController.downloading
                value: SetupController.progress
                Layout.fillWidth: true
            }

            Label {
                visible: SetupController.errorText.length > 0
                text: SetupController.errorText
                color: "#ff8f8f"
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }

            Button {
                text: SetupController.errorText.length > 0 ? "Try Again" : "Download"
                enabled: !SetupController.downloading
                highlighted: true
                Layout.alignment: Qt.AlignHCenter
                onClicked: SetupController.startDownload()
            }
        }
    }
}
