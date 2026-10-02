// One conversation row: user bubble or assistant Markdown answer + actions.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import Lexis

Item {
    id: messageDelegate
    required property var model
    // Row position: only the newest answer may offer "Try harder".
    required property int index
    property int columnWidth: 760
    property bool isLastMessage: false

    signal inspectRequested(var messageModel)

    width: ListView.view ? ListView.view.width : 0
    height: messageColumn.height

    // Live streaming: the partial prefix renders through markdownSafePrefix;
    // finished answers (and history rows) render verbatim.
    readonly property bool live: !messageDelegate.model.isUser && messageDelegate.model.isLive

    // Trims streaming Markdown to the last balanced-markup point (no dangling "**"/links).
    // Single "*" is not balanced: this model emits list bullets as "*   item".
    function markdownSafePrefix(text) {
        var s = text

        // Bold: odd count means the last "**" is unclosed.
        var boldCount = 0
        var lastBold = -1
        var i = 0
        while ((i = s.indexOf("**", i)) !== -1) {
            boldCount++
            lastBold = i
            i += 2
        }
        if (boldCount % 2 === 1)
            s = s.substring(0, lastBold)

        // Inline code: same parity argument.
        var tickCount = 0
        var lastTick = -1
        for (var j = 0; j < s.length; j++) {
            if (s.charAt(j) === "`") {
                tickCount++
                lastTick = j
            }
        }
        if (tickCount % 2 === 1)
            s = s.substring(0, lastTick)

        // Links: hide unclosed "[text" / "[text](url" (but leave "[1]"-style tokens alone).
        var open = s.lastIndexOf("[")
        if (open !== -1) {
            var closeBracket = s.indexOf("]", open)
            if (closeBracket === -1)
                s = s.substring(0, open) // link text still arriving
            else if (s.charAt(closeBracket + 1) === "("
                     && s.indexOf(")", closeBracket) === -1)
                s = s.substring(0, open) // URL still arriving
        }

        return s
    }

    Column {
        id: messageColumn
        width: Math.min(messageDelegate.width, messageDelegate.columnWidth)
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.spacingXS

        // Asymmetric on purpose: the user gets a bubble, the assistant gets the
        // full measure (markdown wants a flush left edge, not a hugging bubble).
        Rectangle {
            id: userBubble
            visible: messageDelegate.model.isUser
            // Hug the text up to 85% of the column, then wrap. No resize
            // animation: the user's text never animates (it juddered).
            width: Math.min(userLabel.implicitWidth + 2 * Theme.spacingM,
                            messageColumn.width * 0.85)
            height: visible ? userLabel.implicitHeight + 2 * Theme.spacingM : 0
            radius: Theme.radiusL
            anchors.right: parent.right
            color: messageDelegate.palette.accent

            Label {
                id: userLabel
                anchors.fill: parent
                anchors.margins: Theme.spacingM
                // PlainText: user-typed * or _ is text, not authored markup.
                textFormat: Text.PlainText
                text: messageDelegate.model.text
                wrapMode: Text.WordWrap
                color: messageDelegate.palette.highlightedText
            }
        }

        // The assistant's answer: full measure, rendered as Markdown.
        Label {
            id: answerLabel
            visible: !messageDelegate.model.isUser
            width: messageColumn.width
            height: visible ? implicitHeight : 0
            textFormat: Text.MarkdownText
            // Finished text renders verbatim: trimming it could hide a tail
            // permanently (a legitimate odd "**" count) instead of for a frame.
            text: messageDelegate.live
                  ? messageDelegate.markdownSafePrefix(messageDelegate.model.text)
                  : messageDelegate.model.text
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeAnswer
            color: messageDelegate.palette.text
            // Without this, rendered links do nothing.
            onLinkActivated: link => Qt.openUrlExternally(link)
        }

        // Answer actions (text labels, not icons): sources, copy, retry.
        Row {
            visible: !messageDelegate.model.isUser && !messageDelegate.live
            spacing: Theme.spacingS

            Button {
                flat: true
                // Only when something was retrieved: a "no source" label
                // would be noise on every conversational reply.
                visible: messageDelegate.model.sources.length > 0
                text: qsTr("Source · %1  ⌄").arg(messageDelegate.model.sources.length)
                font.pixelSize: Theme.fontSizeCaption
                onClicked: messageDelegate.inspectRequested(messageDelegate.model)
            }

            Button {
                flat: true
                text: qsTr("Copy")
                font.pixelSize: Theme.fontSizeCaption
                onClicked: AppController.copyToClipboard(messageDelegate.model.text)
            }

            Button {
                flat: true
                // "Try harder" (SEARCH answers, last row only: that is the row
                // retryLastAnswer() replaces). canRetryLastAnswer covers the rest.
                visible: messageDelegate.model.tool === "search"
                         && messageDelegate.isLastMessage
                         && AppController.canRetryLastAnswer
                text: qsTr("Try harder")
                font.pixelSize: Theme.fontSizeCaption
                onClicked: AppController.retryLastAnswer()
            }
        }
    }
}
