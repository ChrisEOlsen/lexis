// Chat panel: message list bound to AppController.chatModel, plus an
// input row wired to AppController.sendChatMessage(). Disabled (input
// and send) whenever there's no active group, the local model hasn't
// finished loading yet, or a query is already in flight -- chatBusy
// guards against firing a second query while local_llm_chat_completion()
// is still running the first one (see AppController.h's own comment on
// why that's a hard constraint, not just a UX nicety).
//
// ComponentBehavior: Bound -- the session and message delegates below
// reference outer IDs (sessionCombo, messageList); this binds those
// lookups lexically instead of resolving them dynamically at runtime.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

ColumnLayout {
    id: root
    enabled: AppController.activeCorpusId >= 0
    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        ComboBox {
            id: sessionCombo
            Layout.fillWidth: true
            model: AppController.chatSessionModel
            textRole: "title"

            // AppController.activeChatSessionId/activeChatSessionTitle
            // are the source of truth (a session switch can be driven
            // from here, from selectGroup()'s auto-select, or from
            // startNewChat()'s pending -1 state with no row in this
            // model at all yet) -- displayText follows that directly
            // rather than trying to keep this combo's own currentIndex
            // bidirectionally synced to it.
            displayText: AppController.activeChatSessionTitle

            delegate: ItemDelegate {
                id: sessionDelegate
                required property var model
                width: sessionCombo.width
                text: sessionDelegate.model.title
                highlighted: sessionDelegate.model.sessionId === AppController.activeChatSessionId
                onClicked: {
                    AppController.selectChatSession(sessionDelegate.model.sessionId)
                    sessionCombo.popup.close()
                }
            }
        }

        ToolButton {
            text: "New Chat"
            onClicked: AppController.startNewChat()
        }
    }

    Label {
        text: {
            if (AppController.activeCorpusId < 0)
                return "Select a group to start chatting."
            if (!AppController.modelReady)
                return "Loading local model..."
            return "Ask a question about this group's documents."
        }
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: "#555555"
    }

    ListView {
        id: messageList
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        spacing: 10
        model: AppController.chatModel

        // Keeps the newest message in view as messages are appended --
        // ListView doesn't auto-follow by default once content overflows.
        onCountChanged: positionViewAtEnd()

        delegate: Column {
            id: messageDelegate
            required property var model
            width: messageList.width
            spacing: 4

            Rectangle {
                width: Math.min(bubbleLabel.implicitWidth + 24, messageDelegate.width * 0.8)
                height: bubbleLabel.implicitHeight + 16
                radius: 10
                color: messageDelegate.model.isUser ? "#2f6fed" : "#e9e9ec"
                anchors.right: messageDelegate.model.isUser ? parent.right : undefined
                anchors.left: messageDelegate.model.isUser ? undefined : parent.left

                Label {
                    id: bubbleLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    text: messageDelegate.model.text
                    wrapMode: Text.WordWrap
                    color: messageDelegate.model.isUser ? "white" : "black"
                }
            }

            Flow {
                width: messageDelegate.width * 0.8
                anchors.right: messageDelegate.model.isUser ? parent.right : undefined
                anchors.left: messageDelegate.model.isUser ? undefined : parent.left
                visible: !messageDelegate.model.isUser && messageDelegate.model.sources.length > 0
                spacing: 6

                Repeater {
                    model: messageDelegate.model.sources
                    delegate: Label {
                        required property var modelData
                        // chunkId is only present for SEARCH-path sources
                        // (a specific matched passage) -- READ-path
                        // sources are whole documents, no single chunk to
                        // point at.
                        text: modelData.chunkId !== undefined
                              ? modelData.documentName + " (chunk " + modelData.chunkId + ")"
                              : modelData.documentName
                        font.pixelSize: 11
                        color: "#888888"
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        TextField {
            id: questionField
            Layout.fillWidth: true
            enabled: AppController.modelReady && !AppController.chatBusy
            placeholderText: "Ask a question..."
            onAccepted: sendButton.clicked()
        }

        Button {
            id: sendButton
            text: AppController.chatBusy ? "..." : "Send"
            enabled: AppController.modelReady && !AppController.chatBusy && questionField.text.trim().length > 0
            onClicked: {
                AppController.sendChatMessage(questionField.text)
                questionField.text = ""
            }
        }
    }
}
