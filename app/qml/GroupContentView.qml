// Main content area: a narrow document list/drop target on the left
// (accepts drag-and-drop file drops, disabled -- including drops --
// while AppController.busy so a second drop can't start a second
// concurrent rebuild while one is already running) and the chat panel
// filling the rest of the width on the right, so both are visible at
// once while a group is selected.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Rectangle {
    id: root
    color: "white"

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

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            ListView {
                id: documentList
                Layout.preferredWidth: 260
                Layout.fillHeight: true
                clip: true
                enabled: !AppController.busy
                model: AppController.documentModel

                delegate: ItemDelegate {
                    required property var model
                    width: documentList.width
                    text: model.name
                }

                DropArea {
                    anchors.fill: parent
                    enabled: !AppController.busy && AppController.activeCorpusId >= 0
                    onDropped: function (drop) {
                        AppController.ingestFiles(drop.urls)
                    }
                }
            }

            ChatPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }
}
