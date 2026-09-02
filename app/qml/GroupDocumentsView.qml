// Drill-down level 2: the active group's documents -- header (back +
// name), document list (click to view a document's indexed text and
// chunks, "..." menu to remove it), drag-and-drop ingestion, and an
// "Add Documents" file picker. Emits `backRequested`, same "doesn't know
// about its own StackView" shape as GroupsListView.
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

    // NOT `enabled: !AppController.busy` on the root. That froze this
    // whole panel during an ingest -- including the back button, which
    // is the only way to reach OTHER groups, whose chat is deliberately
    // still available mid-ingest. Only the controls that would start a
    // second ingest are gated, individually, below.

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

                        // The at-a-glance corpus numbers: how much of the
                        // index this document actually is. Placeholder
                        // styling on purpose -- a caption, not a second
                        // line competing with the name.
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
                        // Consumes the press, so opening the menu does
                        // not also open the viewer.
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

                // Click opens the document viewer (DocumentViewerDialog)
                // -- the row is a real thing now, not a dead label.
                onClicked: root.openDocumentViewer(docDelegate.model.name)
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
                enabled: AppController.activeCorpusId >= 0 && !AppController.busy
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

        // Same accent-button treatment as GroupsListView's "+ New Group":
        // custom white contentItem because FluentWinUI3 ignores
        // palette.buttonText on highlighted buttons.
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

        // The file picker above cannot select folders (FileDialog and
        // FolderDialog are separate, mutually exclusive pickers -- Qt
        // exposes no combined mode), so folders get their own button.
        // Drag-and-drop remains the one place both work at once.
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
            // ingestFiles walks a dropped-or-picked folder recursively and
            // keeps only supported types -- same path as drag-and-drop.
            AppController.ingestFiles([selectedFolder.toString()])
        }
    }

    // Removal confirm, same stock-dialog pattern as group deletion.
    // Names the passages explicitly: what vanishes is this document's
    // searchable text -- the source file on disk is untouched.
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
