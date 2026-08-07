// Chat panel: header (group name + history + new chat), message list
// bound to AppController.chatModel, and a composer wired to
// AppController.sendChatMessage(). Input is disabled whenever there is no
// active group, the local model hasn't finished loading, or a query is
// already in flight -- chatBusy guards against firing a second query
// while local_llm_chat_completion() is still running the first (see
// AppController.h on why that is a hard constraint, not a UX nicety).
//
// Chat history is a slide-out Drawer listing every session for the active
// group with a relative timestamp, click-to-resume, and per-session
// delete behind a "..." overflow menu.
//
// Everything here that is a control is a stock FluentWinUI3 control. What
// this file still draws by hand is only what no toolkit ships: the chat
// bubbles and the source chips. Those derive their colors from the live
// `palette` rather than from literals, so they track the style.
//
// ComponentBehavior: Bound -- the message and session delegates below
// reference outer IDs; this binds those lookups lexically instead of
// resolving them dynamically at runtime.
//
// Root is a plain Item, not the ColumnLayout itself -- the Drawer and the
// Dialogs are Popups, not layout content, and a Popup declared directly
// inside a ColumnLayout becomes a layout-managed child (undefined
// behavior for its own geometry, per qmllint's Quick.layout-positioning).
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Item {
    id: root
    enabled: AppController.activeCorpusId >= 0

    // Readable measure. At 1300px wide, full-bleed paragraphs run to
    // ~180 characters per line; this caps the conversation column and
    // centers it, the way every chat client does.
    readonly property int columnWidth: 760

    property int pendingDeleteSessionId: -1
    property string pendingDeleteTitle: ""

    // "3 minutes ago" / "2 hours ago" / "Yesterday" / "5 days ago" / a
    // plain date beyond a week -- no new dependency, just Date math.
    function relativeTime(dateTime) {
        if (!dateTime || isNaN(dateTime.getTime()))
            return ""
        var diffMin = Math.floor((new Date() - dateTime) / 60000)
        if (diffMin < 1)
            return "Just now"
        if (diffMin < 60)
            return diffMin + (diffMin === 1 ? " minute ago" : " minutes ago")
        var diffHr = Math.floor(diffMin / 60)
        if (diffHr < 24)
            return diffHr + (diffHr === 1 ? " hour ago" : " hours ago")
        var diffDay = Math.floor(diffHr / 24)
        if (diffDay === 1)
            return "Yesterday"
        if (diffDay < 7)
            return diffDay + " days ago"
        return Qt.formatDate(dateTime, "MMM d, yyyy")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingS

        // -- Header --
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingXS

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    text: AppController.activeCorpusId >= 0 ? AppController.activeCorpusName : qsTr("LEXIS")
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.fontWeightBold
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Label {
                    text: {
                        if (AppController.activeCorpusId < 0)
                            return qsTr("Select a group to start chatting")
                        if (!AppController.modelReady)
                            return qsTr("Loading local model…")
                        return AppController.activeChatSessionTitle
                    }
                    color: root.palette.placeholderText
                    font.pixelSize: Theme.fontSizeCaption
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            Button {
                text: qsTr("History")
                flat: true
                onClicked: historyDrawer.open()
            }

            Button {
                text: qsTr("New chat")
                flat: true
                onClicked: AppController.startNewChat()
            }
        }

        // -- Conversation --
        ListView {
            id: messageList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingS
            clip: true
            spacing: Theme.spacingM
            model: AppController.chatModel

            // Keeps the newest message in view as messages are appended --
            // ListView does not auto-follow once content overflows.
            onCountChanged: positionViewAtEnd()

            // Empty state. An empty ListView with no explanation is the
            // single most common "is this broken?" moment in a chat UI.
            Label {
                anchors.centerIn: parent
                visible: messageList.count === 0 && AppController.activeCorpusId >= 0
                color: root.palette.placeholderText
                text: qsTr("Ask a question about this group's documents.")
            }

            delegate: Item {
                id: messageDelegate
                required property var model
                width: messageList.width
                height: messageColumn.height

                // Fake streaming: the answer is already fully generated by
                // the time it reaches here (local_llm_chat_completion() is
                // synchronous), so this reveals it word-by-word on a fast
                // Timer to give the impression of live generation. Gated
                // on isFresh (see ChatMessageListModel.h) -- history
                // loaded via selectChatSession() must render instantly,
                // never replay the reveal.
                readonly property bool streaming: !messageDelegate.model.isUser && messageDelegate.model.isFresh
                readonly property var streamWords: messageDelegate.model.text.length > 0
                                                   ? messageDelegate.model.text.split(" ") : []
                property int revealedWordCount: streaming ? 0 : streamWords.length

                Timer {
                    interval: 30
                    repeat: true
                    running: messageDelegate.streaming
                             && messageDelegate.revealedWordCount < messageDelegate.streamWords.length
                    onTriggered: messageDelegate.revealedWordCount++
                }

                Column {
                    id: messageColumn
                    width: Math.min(messageDelegate.width, root.columnWidth)
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Theme.spacingXS

                    Rectangle {
                        id: bubble
                        // Hug the text up to 85% of the column, then wrap.
                        // No Behavior on width/height: during the
                        // word-by-word reveal an animated resize fights
                        // the next word's arrival every 30ms, which is
                        // visible as the bubble juddering.
                        width: Math.min(bubbleLabel.implicitWidth + 2 * Theme.spacingM,
                                        messageColumn.width * 0.85)
                        height: bubbleLabel.implicitHeight + 2 * Theme.spacingM
                        radius: Theme.radiusL
                        anchors.right: messageDelegate.model.isUser ? parent.right : undefined
                        anchors.left: messageDelegate.model.isUser ? undefined : parent.left
                        // User speaks in the accent color; the assistant
                        // speaks on a raised neutral layer. Both come from
                        // the palette, so a light mode would follow.
                        color: messageDelegate.model.isUser
                               ? root.palette.accent
                               : Qt.lighter(root.palette.window, Theme.layerRaised)
                        border.width: messageDelegate.model.isUser ? 0 : 1
                        border.color: root.palette.midlight

                        Label {
                            id: bubbleLabel
                            anchors.fill: parent
                            anchors.margins: Theme.spacingM
                            text: messageDelegate.streaming
                                  ? messageDelegate.streamWords.slice(0, messageDelegate.revealedWordCount).join(" ")
                                  : messageDelegate.model.text
                            wrapMode: Text.WordWrap
                            color: messageDelegate.model.isUser
                                   ? root.palette.highlightedText
                                   : root.palette.text
                        }
                    }

                    // Source citations as chips rather than a run of grey
                    // text. Sources land only once the reveal finishes,
                    // matching how a real streaming answer shows citations
                    // after the text rather than mid-reveal.
                    Flow {
                        width: messageColumn.width
                        spacing: Theme.spacingXS
                        visible: !messageDelegate.model.isUser
                                 && messageDelegate.model.sources.length > 0
                                 && !messageDelegate.streaming

                        Repeater {
                            model: messageDelegate.model.sources

                            delegate: Rectangle {
                                id: chip
                                required property var modelData
                                implicitWidth: chipLabel.implicitWidth + 2 * Theme.spacingS
                                implicitHeight: chipLabel.implicitHeight + Theme.spacingS
                                radius: Theme.radiusS
                                // Filled, not outlined. palette.midlight
                                // is white at ~7% alpha, which on this
                                // background produced a border that did
                                // not resolve at all -- the chips read as
                                // loose grey text. A fill at the raised
                                // layer reads as a chip at any size.
                                color: Qt.lighter(chip.palette.window, Theme.layerRaised)

                                Label {
                                    id: chipLabel
                                    anchors.centerIn: parent
                                    // chunkId is only present for
                                    // SEARCH-path sources (a specific
                                    // matched passage) -- READ-path
                                    // sources are whole documents, with no
                                    // single chunk to point at.
                                    text: chip.modelData.chunkId !== undefined
                                          ? chip.modelData.documentName + " · chunk " + chip.modelData.chunkId
                                          : chip.modelData.documentName
                                    font.pixelSize: Theme.fontSizeCaption
                                    color: chip.palette.placeholderText
                                }
                            }
                        }
                    }
                }
            }

            footer: Item {
                width: messageList.width
                height: AppController.chatBusy ? 32 : 0
                visible: AppController.chatBusy

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: Math.max(0, (messageList.width - root.columnWidth) / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacingS

                    // The style's own BusyIndicator, instead of three
                    // hand-animated dots. This is the whole argument for
                    // adopting a component set: the indicator, its timing,
                    // and its motion curve are not this app's problem.
                    BusyIndicator {
                        running: AppController.chatBusy
                        implicitWidth: 20
                        implicitHeight: 20
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Label {
                        text: qsTr("Thinking…")
                        color: root.palette.placeholderText
                        font.pixelSize: Theme.fontSizeCaption
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }

        // -- Composer --
        // Stock TextField plus an accent Button, centered on the same
        // measure as the conversation. The previous hand-built rounded
        // container with a circular send button existed only because
        // Basic's TextField had no design of its own; Fluent's has a
        // proper resting state, focus underline, and disabled state.
        Item {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingXS
            implicitHeight: composerRow.implicitHeight

            RowLayout {
                id: composerRow
                width: Math.min(parent.width, root.columnWidth)
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingS

                TextField {
                    id: questionField
                    Layout.fillWidth: true
                    enabled: AppController.modelReady && !AppController.chatBusy
                    placeholderText: qsTr("Ask a question…")
                    onAccepted: if (sendButton.enabled) sendButton.clicked()
                }

                Button {
                    id: sendButton
                    text: qsTr("Send")
                    highlighted: true
                    enabled: AppController.modelReady && !AppController.chatBusy
                             && questionField.text.trim().length > 0
                    onClicked: {
                        AppController.sendChatMessage(questionField.text)
                        questionField.text = ""
                    }
                }
            }
        }
    }

    // -- Chat history --
    Drawer {
        id: historyDrawer
        edge: Qt.RightEdge
        // Both dimensions are explicit and neither is optional. A Drawer
        // sizes itself from its content like any other Popup
        // (implicitHeight = contentHeight + padding); with no explicit
        // height this collapsed to 26px -- the height of the title label
        // alone -- and squeezed the list below it to height 0. Measured at
        // the time: drawerSize 320x26, listSize 295x0, listContentHeight
        // 332, listCount 7. Qt's own Drawer examples all set both.
        width: Math.min(340, root.width * 0.8)
        height: parent ? parent.height : 0
        // Popup's default parent is its declaring context, not the window;
        // it does not auto-promote to the Overlay the way plain Item
        // children auto-stack. Overlay.overlay is the documented way to
        // get a popup declared inside a layout to render above everything
        // else in its window.
        parent: Overlay.overlay

        // Named edge paddings, not the grouped `padding`: a Drawer style
        // assigns topPadding/bottomPadding/leftPadding/rightPadding
        // itself (it zeroes the edge it is attached to), and those
        // specific properties win over the general one -- confirmed by
        // measurement, `padding: 12` alone left availableWidth at the full
        // drawer width.
        topPadding: Theme.spacingM
        bottomPadding: Theme.spacingM
        leftPadding: Theme.spacingM
        rightPadding: Theme.spacingM

        // FluentWinUI3 does not implement Drawer (it ships 49 controls;
        // Drawer, ScrollBar, ScrollView and Label are not among them), so
        // this falls back to the Basic style and needs its surface stated
        // explicitly. Palette-derived, so it still matches.
        background: Rectangle {
            color: Qt.lighter(historyDrawer.palette.window, Theme.layerCard)
            border.width: 1
            border.color: historyDrawer.palette.midlight
        }

        // No explicit width/height here: the Popup does resize an assigned
        // contentItem, verified by measurement (content 316x736, list
        // 316x709 for a 340x760 drawer with 12px padding). Layouts settle
        // on the polish pass, so sampling geometry in the same call that
        // opens the drawer reads pre-layout values -- that is a
        // measurement artifact, not a sizing bug, and adding redundant
        // explicit sizes to "fix" it only hides where the real constraint
        // is. The Drawer's own `height` above is the one that genuinely
        // cannot be omitted.
        contentItem: ColumnLayout {
            spacing: Theme.spacingS

            Label {
                text: "CHAT HISTORY"
                font.pixelSize: Theme.fontSizeCaption
                font.letterSpacing: 0.6
                color: historyDrawer.palette.placeholderText
                Layout.leftMargin: Theme.spacingS
                Layout.bottomMargin: Theme.spacingXS
            }

            Label {
                visible: historyList.count === 0
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingS
                wrapMode: Text.WordWrap
                color: historyDrawer.palette.placeholderText
                text: qsTr("No chats in this group yet.")
            }

            ListView {
                id: historyList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 2
                reuseItems: false
                model: AppController.chatSessionModel

                delegate: ItemDelegate {
                    id: historyDelegate
                    required property var model
                    width: historyList.width
                    highlighted: historyDelegate.model.sessionId === AppController.activeChatSessionId
                    onClicked: {
                        AppController.selectChatSession(historyDelegate.model.sessionId)
                        historyDrawer.close()
                    }

                    contentItem: RowLayout {
                        spacing: Theme.spacingXS

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Label {
                                text: historyDelegate.model.title
                                color: historyDelegate.palette.text
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            Label {
                                text: root.relativeTime(historyDelegate.model.createdAt)
                                color: historyDelegate.palette.placeholderText
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
                            // not also resume the session.
                            onClicked: sessionMenu.popup()

                            Menu {
                                id: sessionMenu
                                MenuItem {
                                    text: qsTr("Delete chat…")
                                    onTriggered: {
                                        root.pendingDeleteSessionId = historyDelegate.model.sessionId
                                        root.pendingDeleteTitle = historyDelegate.model.title
                                        deleteSessionConfirm.open()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Stock Dialog: no background override, no hand-built footer button.
    // DialogButtonBox is one of the controls the style implements, so the
    // buttons come out correct and keyboard-navigable for free.
    Dialog {
        id: deleteSessionConfirm
        title: qsTr("Delete chat")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Cancel | Dialog.Discard
        onDiscarded: {
            AppController.deleteChatSession(root.pendingDeleteSessionId)
            deleteSessionConfirm.close()
        }

        Label {
            width: deleteSessionConfirm.availableWidth
            wrapMode: Text.WordWrap
            text: qsTr("Delete \"%1\"? This cannot be undone.").arg(root.pendingDeleteTitle)
        }
    }
}
