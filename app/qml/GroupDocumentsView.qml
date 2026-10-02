// Drill-down level 2: the active group's documents (view, remove, ingest).
// Emits `backRequested`; doesn't know about its own StackView.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lexis

Item {
    id: root

    signal backRequested()

    property string pendingRemoveName: ""

    function openDocumentViewer(name) {
        documentViewer.openFor(name, -1)
    }

    DocumentViewerDialog {
        id: documentViewer
    }

    // Root stays enabled during ingest (back must reach other groups' chat);
    // only the controls that would start a second ingest are gated.

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingXS

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingXS

            ToolButton {
                // Text-font glyph, not emoji (emoji render as color stickers).
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

                contentItem: RowLayout {
                    spacing: Theme.spacingXS

                    ColumnLayout {
                        spacing: 0
                        Layout.fillWidth: true

                        Label {
                            text: docDelegate.model.name
                            color: docDelegate.palette.text
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        // Caption-styled index stats (not a second line competing with the name).
                        Label {
                            visible: docDelegate.model.passageCount !== undefined
                            text: docDelegate.model.passageCount + " passages"
                            color: docDelegate.palette.placeholderText
                            font.pixelSize: Theme.fontSizeCaption
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }

                    ToolButton {
                        text: "⋯"
                        font.pixelSize: 16
                        implicitWidth: 28
                        implicitHeight: 28
                        // Consumes the press: opening the menu must not also open the viewer.
                        onClicked: rowMenu.popup()

                        Menu {
                            id: rowMenu

                            MenuItem {
                                text: qsTr("Remove document…")
                                onTriggered: {
                                    root.pendingRemoveName = docDelegate.model.name
                                    removeConfirm.open()
                                }
                            }
                        }
                    }
                }

                // Click opens the document viewer.
                onClicked: root.openDocumentViewer(docDelegate.model.name)
            }

            // Empty state (a blank list reads as "broken").
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
                enabled: AppController.activeCorpusId >= 0 && !AppController.busy
                onDropped: function (drop) {
                    AppController.ingestFiles(drop.urls)
                }
            }
        }

        // Ingest progress (reserved height, so the list doesn't resize).
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

        // White custom contentItem: the style ignores palette.buttonText when highlighted.
        Button {
            id: addDocumentsButton
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingXS
            text: qsTr("+  Add Documents")
            highlighted: true
            enabled: !AppController.busy
            contentItem: Text {
                text: addDocumentsButton.text
                font: addDocumentsButton.font
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            onClicked: addDocumentsDialog.open()
        }

        // Separate button: Qt has no combined file+folder picker.
        Button {
            id: addFolderButton
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingXS
            text: qsTr("+  Add Folder")
            highlighted: true
            enabled: !AppController.busy
            contentItem: Text {
                text: addFolderButton.text
                font: addFolderButton.font
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            onClicked: addFolderDialog.open()
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

    FolderDialog {
        id: addFolderDialog
        title: qsTr("Add Folder")
        onAccepted: {
            // ingestFiles walks folders recursively (same as drag-and-drop).
            AppController.ingestFiles([selectedFolder.toString()])
        }
    }

    // Removal confirm (source file on disk is untouched).
    Dialog {
        id: removeConfirm
        title: qsTr("Remove document")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Cancel | Dialog.Discard
        onDiscarded: {
            AppController.removeDocument(root.pendingRemoveName)
            removeConfirm.close()
        }

        ColumnLayout {
            width: removeConfirm.availableWidth
            spacing: Theme.spacingS

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Remove \"%1\" from this group?").arg(root.pendingRemoveName)
                font.weight: Theme.fontWeightBold
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Its text becomes unsearchable here and its passages stop appearing in answers. The original file on disk is not deleted. This cannot be undone.")
            }
        }
    }
}
