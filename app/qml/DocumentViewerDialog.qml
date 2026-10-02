// Document viewer: stored text + chunks, opened from the source inspector or the
// document list. Near-fullscreen (the text is the point) and read-only throughout.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Dialog {
    id: viewer

    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? Math.round(parent.width * 0.92) : 0
    height: parent ? Math.round(parent.height * 0.9) : 0
    standardButtons: Dialog.Close
    title: qsTr("Document")

    // Populated by openFor() before open(), so failures never open half-populated.
    property string documentName: ""
    property string documentText: ""
    property var chunks: []          // [{chunkId, text, tokenCount}, ...]
    property int focusChunkId: -1    // cited chunk to scroll to + highlight
    property int selectedTab: 0      // 0 = extracted text, 1 = passages

    // Stats label, built here when the caller passes none.
    property string statsLabel: ""

    function openFor(name, chunkId) {
        var doc = AppController.openDocument(name)
        if (!doc || doc.text === undefined) {
            // openDocument returns {} on failure: surface it, don't open an empty shell.
            AppController.showMessage(qsTr("Could not open \"%1\" -- it may have been removed.").arg(name))
            return
        }
        viewer.documentName = name
        viewer.documentText = doc.text !== undefined ? doc.text : ""
        viewer.chunks = doc.chunks !== undefined ? doc.chunks : []
        viewer.statsLabel = qsTr("%1 passages").arg(viewer.chunks.length)
        if (chunkId !== undefined && chunkId >= 0) {
            viewer.focusChunkId = chunkId
            viewer.selectedTab = 1
        } else {
            viewer.focusChunkId = -1
            viewer.selectedTab = 0
        }
        open()
        // After open() and focusChunkId: the count-driven scroll may have fired stale.
        chunkList.scrollToFocusChunk()
    }

    // Paragraph blocks (a single big Label would defeat ListView virtualization).
    readonly property var textBlocks: viewer.documentText.length === 0
                                      ? [] : viewer.documentText.split(/\n{2,}/)

    contentItem: ColumnLayout {
        spacing: Theme.spacingS

        // Header.
        Label {
            text: viewer.documentName
            font.weight: Theme.fontWeightBold
            font.pixelSize: Theme.fontSizeTitle
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        Label {
            text: viewer.statsLabel
            color: viewer.palette.placeholderText
            font.pixelSize: Theme.fontSizeCaption
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        // Two checked buttons, not a TabBar: a two-way toggle keeping the style's chrome.
        RowLayout {
            spacing: Theme.spacingXS
            Layout.fillWidth: true

            Button {
                flat: true
                checked: viewer.selectedTab === 0
                text: qsTr("Extracted text")
                onClicked: viewer.selectedTab = 0
            }
            Button {
                flat: true
                checked: viewer.selectedTab === 1
                text: qsTr("Passages · %1").arg(viewer.chunks.length)
                onClicked: viewer.selectedTab = 1
            }
            Item { Layout.fillWidth: true }
            Button {
                flat: true
                text: qsTr("Copy")
                font.pixelSize: Theme.fontSizeCaption
                // Copies whichever tab is showing.
                onClicked: {
                    if (viewer.selectedTab === 0) {
                        AppController.copyToClipboard(viewer.documentText)
                    } else {
                        var joined = []
                        for (var i = 0; i < viewer.chunks.length; i++) {
                            joined.push(viewer.chunks[i].text)
                        }
                        AppController.copyToClipboard(joined.join("\n\n"))
                    }
                }
            }
        }

        // -- Extracted text (PlainText: source shown verbatim; bare ListView, no ScrollView) --
        ListView {
            id: textList
            visible: viewer.selectedTab === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.spacingS
            model: viewer.textBlocks

            delegate: Label {
                required property var modelData
                width: textList.width
                text: modelData
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: viewer.palette.text
            }

            Label {
                anchors.centerIn: parent
                visible: textList.count === 0
                color: viewer.palette.placeholderText
                text: qsTr("No extracted text.")
            }
        }

        // -- Passages --
        ListView {
            id: chunkList
            visible: viewer.selectedTab === 1
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.spacingXS
            reuseItems: false
            model: viewer.chunks

            delegate: Rectangle {
                id: chunkCard
                required property var modelData
                width: chunkList.width
                implicitHeight: chunkColumn.implicitHeight + 2 * Theme.spacingM
                radius: Theme.radiusM
                color: Qt.lighter(chunkCard.palette.window, Theme.layerCard)
                border.width: modelData.chunkId === viewer.focusChunkId ? 2 : 1
                border.color: modelData.chunkId === viewer.focusChunkId
                              ? chunkCard.palette.accent : chunkCard.palette.midlight

                ColumnLayout {
                    id: chunkColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spacingM
                    spacing: Theme.spacingXS

                    RowLayout {
                        spacing: Theme.spacingS
                        Layout.fillWidth: true

                        Label {
                            text: qsTr("chunk %1").arg(chunkCard.modelData.chunkId)
                            font.weight: Theme.fontWeightBold
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Label {
                            visible: chunkCard.modelData.tokenCount !== undefined
                            text: qsTr("%1 tokens").arg(chunkCard.modelData.tokenCount)
                            color: chunkCard.palette.placeholderText
                            font.pixelSize: Theme.fontSizeCaption
                        }

                        Button {
                            flat: true
                            text: qsTr("Copy")
                            font.pixelSize: Theme.fontSizeCaption
                            onClicked: AppController.copyToClipboard(chunkCard.modelData.text)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: chunkCard.modelData.text !== undefined ? chunkCard.modelData.text : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: chunkList.count === 0
                color: viewer.palette.placeholderText
                text: qsTr("No passages.")
            }

            // Deferred scroll to the cited chunk (positioning before delegates exist lands
            // nowhere). Also called from openFor(): same-length reopens never fire onCountChanged.
            function scrollToFocusChunk() {
                if (viewer.focusChunkId < 0 || chunkList.count === 0) {
                    return
                }
                Qt.callLater(function () {
                    for (var i = 0; i < viewer.chunks.length; i++) {
                        if (viewer.chunks[i].chunkId === viewer.focusChunkId) {
                            chunkList.currentIndex = i
                            chunkList.positionViewAtIndex(i, ListView.Center)
                            return
                        }
                    }
                })
            }

            onCountChanged: chunkList.scrollToFocusChunk()
        }
    }
}