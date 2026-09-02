// Document viewer: one document's stored text and its chunks, opened
// from the Source inspector's passage cards (focused on the cited
// chunk) or from the document list (whole document). Near-fullscreen
// like the source inspector, for the same reason -- the text is the
// point, and a letterbox turns reading into a scroll-race.
//
// Two tabs:
//   - "Extracted text": the document's full stored text, split into
//     paragraph blocks in a virtualized ListView -- a 900-page manual
//     is over a megabyte of text, and one giant Label would defeat
//     virtualization and stall the scroll.
//   - "Passages": the chunk cards search actually retrieves, in chunk
//     order. Opening from a source card lands here, scrolled to and
//     highlighting the cited chunk -- the "is the answer actually
//     supported by this passage?" question gets answered in the
//     passage's surrounding context, one click from the answer.
//
// Everything is read-only; the viewer deliberately has no editing, no
// re-indexing, no "open the original file" (the original file's path is
// not stored anywhere -- the indexed text IS the source of truth here,
// which is also what makes this honest: it shows exactly what search
// sees).
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

    // Populated by openFor() before open(): the viewer's whole payload
    // in one AppController call, so a document that can't be read never
    // opens half-populated.
    property string documentName: ""
    property string documentText: ""
    property var chunks: []          // [{chunkId, text, tokenCount}, ...]
    property int focusChunkId: -1    // cited chunk to scroll to + highlight
    property int selectedTab: 0      // 0 = extracted text, 1 = passages

    // A short label the caller can pass ("12 passages · 8,412 tokens");
    // built here when absent so both entry points share the code.
    property string statsLabel: ""

    function openFor(name, chunkId) {
        var doc = AppController.openDocument(name)
        if (!doc || doc.text === undefined) {
            // openDocument returns {} on failure -- surface it rather
            // than opening an empty shell over a real click.
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
        // After open(), and after focusChunkId is set: the assignment to
        // `chunks` above may already have fired onCountChanged with the
        // PREVIOUS focus id (or not fired at all, on a same-length
        // reopen), so this is the call that is always correct.
        chunkList.scrollToFocusChunk()
    }

    // Paragraph blocks for the extracted-text tab, recomputed whenever
    // the loaded text changes. A single big Label defeats ListView
    // virtualization; ~paragraph-sized blocks keep the list cheap no
    // matter how long the document is.
    readonly property var textBlocks: viewer.documentText.length === 0
                                      ? [] : viewer.documentText.split(/\n{2,}/)

    contentItem: ColumnLayout {
        spacing: Theme.spacingS

        // Header: name + one stats line.
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

        // Tab bar. Two buttons, not a TabBar: this is a two-way toggle
        // where the checked state IS the selection, and stock flat
        // buttons with `checked` keep the style's own press/hover
        // treatment without hand-drawing a tab chrome no other surface
        // in the app uses.
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
                // Copies whichever tab is showing -- the extracted text
                // or the chunk texts joined in order.
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

        // -- Extracted text --
        // PlainText, not Markdown: this is source material shown
        // verbatim, and any punctuation in it must not reformat. A bare
        // ListView (its own flickable -- a ScrollView wrapper would
        // stack two), virtualized over paragraph blocks.
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

            // Scroll to the cited chunk once the list is laid out --
            // positionViewAtIndex before the delegates exist lands
            // nowhere (the same deferred-positioning discipline as the
            // message list's followTail).
            //
            // Called from openFor(), not only from onCountChanged:
            // reopening the SAME document at a different cited chunk
            // replaces `chunks` with an array of the same length, so
            // count never changes and a count-driven scroll would leave
            // the view sitting on the previous chunk while the highlight
            // moved off-screen.
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