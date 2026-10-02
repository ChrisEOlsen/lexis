// Root window: sidebar + chat panel, with notify() shown as a dialog.
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

    // Sidebar collapse (pure UI state). Animates a plain property: Behavior on
    // attached Layout properties is not reliably supported.
    property bool sidebarCollapsed: false
    property real sidebarWidth: sidebarCollapsed ? 0 : 260
    Behavior on sidebarWidth {
        NumberAnimation { duration: Theme.durationNav; easing.type: Easing.OutCubic }
    }

    // No explicit color: the style's window background matches dialogs/popups for free.

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

        // Explicit widths on dialog AND label: without both, a binding loop jitters
        // the dialog as it opens (Dialog <-> WordWrap label implicitWidth).
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
            // Fade with the width so content slides away rather than crushing.
            opacity: window.sidebarWidth / 260
            // At width 0 the item must not paint or eat layout spacing.
            visible: window.sidebarWidth > 0.5
            clip: true
        }

        ChatPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            onSidebarToggleRequested: window.sidebarCollapsed = !window.sidebarCollapsed
        }
    }

    // First-run setup overlay (bundle mode, models missing). Covers the window until
    // the download lands, then AppController starts the skipped model load.
    Connections {
        target: SetupController
        function onSetupComplete() {
            AppController.retryModelLoad()
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: SetupController.required || SetupController.downloading
        // Opaque: the live UI bleeding through would look broken.
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
