// Root window. Lays out the group sidebar against the content view and
// shows AppController's notify() signal as a dismissible dialog -- the
// one place both children funnel user-facing messages through.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

ApplicationWindow {
    id: window
    visible: true
    width: 1000
    height: 650
    title: "LEXIS"

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
        standardButtons: Dialog.Ok
        modal: true
        anchors.centerIn: parent
        property alias text: messageLabel.text

        Label {
            id: messageLabel
            wrapMode: Text.WordWrap
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 1

        GroupSidebar {
            Layout.preferredWidth: 240
            Layout.fillHeight: true
        }

        GroupContentView {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
