// Main content area: shows the active group's documents and accepts
// drag-and-drop file drops to add more. Disabled (including drops)
// while AppController.busy -- also what prevents a second drop from
// starting a second concurrent rebuild while one is already running.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Rectangle {
    id: root
    color: "white"
    enabled: !AppController.busy

    DropArea {
        anchors.fill: parent
        enabled: AppController.activeCorpusId >= 0
        onDropped: function (drop) {
            AppController.ingestFiles(drop.urls)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            text: AppController.activeCorpusId >= 0 ? AppController.activeCorpusName : "Select a group"
            font.pixelSize: 20
            font.bold: true
        }

        Label {
            text: AppController.activeCorpusId >= 0 ? AppController.statusText : "Select a group to see its documents."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            color: "#555555"
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: AppController.documentModel

            delegate: ItemDelegate {
                required property var model
                width: parent ? parent.width : 0
                text: model.name
            }
        }
    }
}
