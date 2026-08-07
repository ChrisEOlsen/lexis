// Drill-down level 2: the active group's documents -- header (back +
// name), document list, drag-and-drop ingestion, and an "Add Documents"
// file picker. Emits `backRequested`, same "doesn't know about its own
// StackView" shape as GroupsListView.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lexis

Item {
    id: root

    signal backRequested()

    enabled: !AppController.busy

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingXS

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingXS

            ToolButton {
                // U+2039, a text-font glyph. Not an emoji: emoji
                // codepoints resolve to Apple Color Emoji and render as
                // full-color stickers against flat chrome.
                text: "‹"
                font.pixelSize: Theme.fontSizeTitle
                implicitWidth: 28
                implicitHeight: 28
                onClicked: root.backRequested()
            }

            Label {
                text: AppController.activeCorpusName
                font.weight: Theme.fontWeightBold
                Layout.fillWidth: true
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }

        Label {
            text: "DOCUMENTS"
            font.pixelSize: Theme.fontSizeCaption
            font.letterSpacing: 0.6
            color: root.palette.placeholderText
            Layout.leftMargin: Theme.spacingS
            Layout.topMargin: Theme.spacingXS
        }

        ListView {
            id: documentList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: AppController.documentModel
            reuseItems: false

            delegate: ItemDelegate {
                id: docDelegate
                required property var model
                width: documentList.width
                // Not clickable: there is no document detail view yet, so
                // the row is presentational. hoverEnabled/press feedback
                // would promise an interaction that does not exist.
                enabled: false

                contentItem: Label {
                    // docDelegate.model, not parent.model: `parent` is
                    // whatever visual parent the Control gives its
                    // contentItem, which is only correct by accident.
                    text: docDelegate.model.name
                    color: docDelegate.palette.text
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Empty state -- previously the list just showed nothing,
            // which reads as "broken" rather than "drop files here".
            Label {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.spacingM
                visible: documentList.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: root.palette.placeholderText
                text: qsTr("No documents yet.\nDrop files here, or use Add Documents.")
            }

            DropArea {
                anchors.fill: parent
                enabled: AppController.activeCorpusId >= 0
                onDropped: function (drop) {
                    AppController.ingestFiles(drop.urls)
                }
            }
        }

        // Ingest progress. Reserved height, so the list above does not
        // resize every time the status text appears and disappears.
        Label {
            text: AppController.statusText
            visible: AppController.busy
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            color: root.palette.placeholderText
            font.pixelSize: Theme.fontSizeCaption
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: AppController.busy
            indeterminate: true
        }

        Button {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingXS
            text: qsTr("Add Documents")
            highlighted: true
            onClicked: addDocumentsDialog.open()
        }
    }

    FileDialog {
        id: addDocumentsDialog
        title: qsTr("Add Documents")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            "Supported documents (*.txt *.csv *.docx *.pdf *.png *.jpg *.jpeg *.tiff *.tif *.bmp)",
            "All files (*)"
        ]
        onAccepted: {
            var urls = []
            for (var i = 0; i < selectedFiles.length; i++) {
                urls.push(selectedFiles[i].toString())
            }
            AppController.ingestFiles(urls)
        }
    }
}
