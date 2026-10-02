// Source inspector: what produced this answer, and what it read. Near-fullscreen:
// passages run to hundreds of words and are the point of the dialog.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Dialog {
    id: sourceInspector
    title: qsTr("Source")
    modal: true
    // Parented to the overlay: the size is expressed against parent.width/height.
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? Math.round(parent.width * 0.92) : 0
    height: parent ? Math.round(parent.height * 0.9) : 0
    standardButtons: Dialog.Close

    // Populated by openFor() (one shared inspector, not a Popup per message row).
    property string inspectAnswer: ""
    property string inspectTool: ""
    property var inspectSources: []
    // SEARCH provenance (empty for CHAT/SUMMARY, which hides that section).
    property string inspectSearchQuery: ""
    property string inspectSearchTerms: ""
    // DocumentViewerDialog instance (owned by ChatPanel) for the per-card Open button.
    property var documentViewer: null

    function openFor(messageModel) {
        sourceInspector.inspectAnswer = messageModel.text
        sourceInspector.inspectTool = messageModel.tool
        sourceInspector.inspectSources = messageModel.sources
        sourceInspector.inspectSearchQuery = messageModel.searchQuery
        sourceInspector.inspectSearchTerms = messageModel.searchTerms
        sourceInspector.open()
    }

    // Human wording for a stored tool token (storage stays short/lowercase).
    function toolHeadline(tool) {
        if (tool === "search")
            return qsTr("Lexical search (BM25)")
        if (tool === "summary")
            return qsTr("Group summary")
        if (tool === "read")
            return qsTr("Direct document read")
        if (tool === "chat")
            return qsTr("No tool called")
        return qsTr("Unknown")
    }

    function toolDetail(tool) {
        if (tool === "search")
            return qsTr("The question was turned into search terms and matched against indexed passages. The passages below are what the answer was generated from, in rank order.")
        if (tool === "summary")
            return qsTr("This question was about the collection as a whole, so it was answered from a generated overview of the group rather than by re-reading the documents. The overview is built once per group, cached, and rebuilt when the document count changes. It is a representative sample of each document, not their full text, so it can miss a topic that appears only in an unsampled section.")
        if (tool === "read")
            return qsTr("Keyword search could not reach this question -- its words matched nothing useful -- so the answer was written from the documents' full text instead, within a context limit of %1 tokens. Only groups small enough to fit take this path; the documents below are everything that was read, in full.")
                   .arg(AppController.contextTokenLimit)
        if (tool === "chat")
            return qsTr("The router classified this as conversation rather than a request for information, so no retrieval ran and no documents were consulted. The reply came from the model and the conversation so far.")
        return qsTr("This answer was recorded before the tool was tracked, so which tool ran is not known. Any citations shown came from the search pipeline.")
    }

    ScrollView {
        id: inspectorScroll
        anchors.fill: parent
        // Without this the view also scrolls sideways.
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: inspectorScroll.availableWidth
            spacing: Theme.spacingS

            // -- How it was answered --
            Label {
                text: "TOOL CALLED"
                font.pixelSize: Theme.fontSizeCaption
                font.letterSpacing: 0.6
                color: sourceInspector.palette.placeholderText
            }

            Label {
                Layout.fillWidth: true
                text: sourceInspector.toolHeadline(sourceInspector.inspectTool)
                font.weight: Theme.fontWeightBold
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: sourceInspector.palette.placeholderText
                text: sourceInspector.toolDetail(sourceInspector.inspectTool)
            }

            // -- The answer it produced --
            Label {
                text: "ANSWER"
                Layout.topMargin: Theme.spacingM
                font.pixelSize: Theme.fontSizeCaption
                font.letterSpacing: 0.6
                color: sourceInspector.palette.placeholderText
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: inspectAnswerLabel.implicitHeight + 2 * Theme.spacingM
                radius: Theme.radiusM
                color: Qt.lighter(sourceInspector.palette.window, Theme.layerRaised)

                Label {
                    id: inspectAnswerLabel
                    anchors.fill: parent
                    anchors.margins: Theme.spacingM
                    text: sourceInspector.inspectAnswer
                    textFormat: Text.MarkdownText
                    wrapMode: Text.WordWrap
                    onLinkActivated: link => Qt.openUrlExternally(link)
                }
            }

            // -- How it searched --
            Label {
                visible: sourceInspector.inspectSearchTerms.length > 0
                Layout.topMargin: Theme.spacingM
                text: qsTr("SEARCH QUERY")
                font.pixelSize: Theme.fontSizeCaption
                font.letterSpacing: 0.6
                color: sourceInspector.palette.placeholderText
            }

            Rectangle {
                visible: sourceInspector.inspectSearchTerms.length > 0
                Layout.fillWidth: true
                implicitHeight: searchQueryColumn.implicitHeight + 2 * Theme.spacingM
                radius: Theme.radiusM
                color: Qt.lighter(sourceInspector.palette.window, Theme.layerRaised)

                ColumnLayout {
                    id: searchQueryColumn
                    anchors.fill: parent
                    anchors.margins: Theme.spacingM
                    spacing: Theme.spacingS

                    Label {
                        visible: sourceInspector.inspectSearchQuery.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Rewritten question: %1").arg(sourceInspector.inspectSearchQuery)
                        color: sourceInspector.palette.placeholderText
                    }

                    // The literal lexical query: monospace, not prose styling.
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: sourceInspector.inspectSearchTerms
                        font.family: "Menlo"
                        font.pixelSize: Theme.fontSizeCaption
                    }
                }
            }

            // -- What it read --
            Label {
                visible: sourceInspector.inspectSources.length > 0
                Layout.topMargin: Theme.spacingM
                text: {
                    if (sourceInspector.inspectTool === "summary")
                        return qsTr("SUMMARY AND COVERAGE · %1").arg(sourceInspector.inspectSources.length)
                    if (sourceInspector.inspectTool === "read")
                        return qsTr("DOCUMENTS AVAILABLE · %1").arg(sourceInspector.inspectSources.length)
                    return qsTr("RETRIEVED PASSAGES · %1").arg(sourceInspector.inspectSources.length)
                }
                font.pixelSize: Theme.fontSizeCaption
                font.letterSpacing: 0.6
                color: sourceInspector.palette.placeholderText
            }

            Repeater {
                model: sourceInspector.inspectSources

                delegate: Rectangle {
                    id: sourceCard
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: cardColumn.implicitHeight + 2 * Theme.spacingM
                    radius: Theme.radiusM
                    color: Qt.lighter(sourceCard.palette.window, Theme.layerCard)
                    border.width: 1
                    border.color: sourceCard.palette.midlight

                    ColumnLayout {
                        id: cardColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.spacingM
                        spacing: Theme.spacingXS

                        // Identity line + Open (at the cited chunk; SUMMARY rows have
                        // no chunk, so they open at the top).
                        RowLayout {
                            spacing: Theme.spacingXS
                            Layout.fillWidth: true

                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                font.weight: Theme.fontWeightBold
                                text: {
                                    var name = sourceCard.modelData.documentName
                                    if (sourceCard.modelData.chunkId === undefined)
                                        return name
                                    return qsTr("%1 · chunk %2").arg(name).arg(sourceCard.modelData.chunkId)
                                }
                            }

                            Button {
                                flat: true
                                font.pixelSize: Theme.fontSizeCaption
                                text: qsTr("Open")
                                onClicked: sourceInspector.documentViewer.openFor(sourceCard.modelData.documentName,
                                                                  sourceCard.modelData.chunkId)
                            }
                        }

                        // Retrieval metadata, shown only when present.
                        Label {
                            Layout.fillWidth: true
                            visible: sourceCard.modelData.score !== undefined
                            font.pixelSize: Theme.fontSizeCaption
                            color: sourceCard.palette.placeholderText
                            text: {
                                var parts = []
                                if (sourceCard.modelData.score !== undefined)
                                    parts.push(qsTr("BM25 score %1").arg(sourceCard.modelData.score.toFixed(3)))
                                if (sourceCard.modelData.tokenCount !== undefined)
                                    parts.push(qsTr("%1 tokens").arg(sourceCard.modelData.tokenCount))
                                return parts.join("   ·   ")
                            }
                        }

                        // The passage itself, quoted verbatim (PlainText: punctuation must
                        // not be reinterpreted as formatting).
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spacingXS
                            visible: text.length > 0
                            text: sourceCard.modelData.text !== undefined
                                  ? sourceCard.modelData.text : ""
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // Older answers predate persisted passage text (ids only, no cards).
            Label {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingS
                visible: sourceInspector.inspectTool === "search" && sourceInspector.inspectSources.length > 0
                         && sourceInspector.inspectSources[0].text === undefined
                wrapMode: Text.WordWrap
                color: sourceInspector.palette.placeholderText
                text: qsTr("This answer predates passage text being stored, so only the identifiers above are available for it. New answers include the full passage.")
            }
        }
    }
}
