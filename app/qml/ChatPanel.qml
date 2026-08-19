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
    // NOT `enabled: activeCorpusId >= 0` on the whole panel. Disabling the
    // root disables every descendant -- a child cannot re-enable itself,
    // since effective enabled is the AND of the chain -- which greyed out
    // the startup loading indicator, the one thing that must look alive
    // while the model loads and no group is selected yet. The controls that
    // genuinely need a group now say so individually.

    // Readable measure. At 1300px wide, full-bleed paragraphs run to
    // ~180 characters per line; this caps the conversation column and
    // centers it, the way every chat client does.
    readonly property int columnWidth: 760

    // Whether the group being VIEWED is the one currently ingesting.
    // Chat is replaced by the progress panel below for exactly that
    // group; switching to any other group brings the normal chat back
    // immediately, mid-ingest.
    readonly property bool groupIngesting: AppController.activeCorpusId >= 0
                                           && AppController.activeCorpusId === AppController.ingestingCorpusId

    // Emitted by the header's "☰" button; Main.qml owns the actual
    // collapsed/expanded state.
    signal sidebarToggleRequested()

    property int pendingDeleteSessionId: -1
    property string pendingDeleteTitle: ""

    // One shared inspector, populated on open, rather than a Popup per
    // message delegate: a modal per row would instantiate one full-window
    // dialog for every message in the conversation.
    property string inspectAnswer: ""
    property string inspectTool: ""
    property var inspectSources: []
    // SEARCH-path provenance: the AI-rewritten standalone question (empty
    // when it matched the user's wording) and the lexical terms BM25
    // actually searched. Empty for CHAT/SUMMARY and pre-tracking rows,
    // which hides their section below.
    property string inspectSearchQuery: ""
    property string inspectSearchTerms: ""

    function openSourceInspector(messageModel) {
        root.inspectAnswer = messageModel.text
        root.inspectTool = messageModel.tool
        root.inspectSources = messageModel.sources
        root.inspectSearchQuery = messageModel.searchQuery
        root.inspectSearchTerms = messageModel.searchTerms
        sourceInspector.open()
    }

    // Human wording for a stored tool token. The stored values stay short
    // and lowercase ("search"/"read"/"chat") so this presentation can
    // change without rewriting rows -- see QueryWorker's toolName().
    function toolHeadline(tool) {
        if (tool === "search")
            return qsTr("Lexical search (BM25)")
        if (tool === "summary")
            return qsTr("Group summary")
        if (tool === "read")
            return qsTr("Direct document read (retired)")
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
            return qsTr("An earlier version of this app answered broad questions by feeding whole documents to the local model, within a context limit of %1 tokens. That path has been replaced by the cached group summary; this answer predates the change.")
                   .arg(AppController.contextTokenLimit)
        if (tool === "chat")
            return qsTr("The router classified this as conversation rather than a request for information, so no retrieval ran and no documents were consulted. The reply came from the model and the conversation so far.")
        return qsTr("This answer was recorded before the tool was tracked, so which tool ran is not known. Any citations shown came from the search pipeline.")
    }

    // Trims a partially-revealed Markdown string back to the last point
    // where its inline markup is balanced, so the word-by-word reveal
    // never paints a dangling "**" or an unclosed link as literal text.
    //
    // Without this, rendering the revealed prefix as Markdown shows the
    // raw asterisks of a half-arrived "**Operator, Class D:**" for a
    // frame or two before they resolve into bold -- the same one-to-two
    // frame flicker class that made the old hover styling feel janky. The
    // visible effect of trimming is that a bold phrase appears as a unit
    // instead of character-by-character, which reads as intentional.
    //
    // Deliberately handles only "**", backtick code spans and "[...](...)"
    // links. Single "*" is NOT balanced here: this model emits list items
    // as "*   item", so a lone leading asterisk is a bullet, and treating
    // it as an unterminated italic would swallow every list line until
    // its successor arrived.
    function markdownSafePrefix(text) {
        var s = text

        // Bold: an odd number of "**" means the last one is unclosed.
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

        // Inline code: same parity argument, single character.
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

        // Links: hide "[text" and "[text](partial-url" until they close,
        // otherwise the raw URL is briefly visible. Checked in two steps
        // so an ordinary bracketed token that is not a link -- "[1]", a
        // citation marker -- is left alone rather than being withheld
        // until the end of the reveal.
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

            // Sidebar collapse toggle. Pure UI state owned by Main.qml;
            // this button only requests the flip. Fixed square with zero
            // padding so the glyph sits dead-centre and the left/right
            // breathing room around the button reads symmetric.
            ToolButton {
                text: "☰"
                font.pixelSize: 22
                padding: 0
                implicitWidth: 38
                implicitHeight: 38
                Layout.rightMargin: Theme.spacingXS
                onClicked: root.sidebarToggleRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Show or hide the sidebar")
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    // "Select a group", not "LEXIS" -- the window frame
                    // already says LEXIS, and the app name where the
                    // group name belongs reads as a broken state.
                    text: AppController.activeCorpusId >= 0 ? AppController.activeCorpusName : qsTr("Select a group")
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.fontWeightBold
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Label {
                    // Deliberately no longer reports model loading. A
                    // caption in the corner while the whole chat area sits
                    // empty and disabled reads as a frozen window; the
                    // loading state now owns the centre of the conversation
                    // area instead, where the user is already looking.
                    text: AppController.activeCorpusId < 0
                          ? qsTr("Select a group to start chatting")
                          : AppController.activeChatSessionTitle
                    color: root.palette.placeholderText
                    font.pixelSize: Theme.fontSizeCaption
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            Button {
                id: historyButton
                flat: true
                enabled: AppController.activeCorpusId >= 0
                onClicked: historyDrawer.open()
                // Custom contentItem so the glyph can be larger than the
                // label while both stay vertically centred as one row --
                // a single text string can only render at one size.
                // Text glyphs, not an icon font: matches the app's
                // existing "⋯"/"⌄" usage, no bundled assets. Labels take
                // their default palette colour, which tracks the
                // button's enabled/disabled state on its own.
                contentItem: Row {
                    spacing: Theme.spacingXS
                    Label {
                        text: "◷"
                        font.pixelSize: 18
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Label {
                        text: qsTr("History")
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            Button {
                id: newChatButton
                flat: true
                enabled: AppController.activeCorpusId >= 0
                onClicked: AppController.startNewChat()
                contentItem: Row {
                    spacing: Theme.spacingXS
                    Label {
                        text: "✎"
                        font.pixelSize: 18
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Label {
                        text: qsTr("New chat")
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }

        // -- Ingestion in progress (replaces the conversation for the
        // one group currently ingesting) --
        ColumnLayout {
            visible: root.groupIngesting
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacingL

            Item { Layout.fillHeight: true }

            Label {
                text: qsTr("Ingestion in progress")
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Theme.fontWeightBold
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                text: AppController.ingestStatusText
                color: root.palette.placeholderText
                Layout.alignment: Qt.AlignHCenter
            }

            ProgressBar {
                Layout.preferredWidth: Math.min(420, root.width - 2 * Theme.spacingXL)
                Layout.alignment: Qt.AlignHCenter
                value: AppController.ingestProgress
                // The animation duration comes from C++: short real steps
                // while files are read, then one long glide through the
                // index rebuild's estimated duration (the rebuild itself
                // reports no progress -- see IngestWorker.h).
                Behavior on value {
                    NumberAnimation {
                        duration: AppController.ingestAnimMs
                        easing.type: Easing.OutQuad
                    }
                }
            }

            Label {
                text: qsTr("You can chat with other groups while this finishes.")
                color: root.palette.placeholderText
                font.pixelSize: Theme.fontSizeCaption
                Layout.alignment: Qt.AlignHCenter
            }

            Button {
                text: qsTr("Cancel")
                Layout.alignment: Qt.AlignHCenter
                // Lossless: the rebuild only replaces the group's data at
                // its very last step, so cancelling leaves the group as it
                // was before the drop (see AppController::cancelIngest()).
                onClicked: AppController.cancelIngest()
            }

            Item { Layout.fillHeight: true }
        }

        // -- Conversation --
        ListView {
            id: messageList
            visible: !root.groupIngesting
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingS
            clip: true
            spacing: Theme.spacingM
            model: AppController.chatModel

            // Keeps the newest message in view. Two mechanisms, both
            // needed:
            //
            // 1. positionViewAtEnd() on count changes alone under-scrolls:
            //    a new message's delegate height (wrapped text) settles
            //    AFTER countChanged fires, so the jump lands short of the
            //    real end. Deferring via Qt.callLater(), and re-following
            //    on contentHeight changes, tracks the true bottom as the
            //    layout finishes.
            //
            // 2. followTail: auto-scroll must stop the moment the user
            //    scrolls up to reread something (yanking the view down
            //    while they read is worse than not following), and resume
            //    when they return to the bottom -- or when they send a
            //    new message, which is an explicit "I'm at the front of
            //    the conversation again".
            property bool followTail: true
            onCountChanged: {
                followTail = true
                Qt.callLater(positionViewAtEnd)
            }
            onContentHeightChanged: {
                if (followTail) {
                    Qt.callLater(positionViewAtEnd)
                }
            }
            onMovementStarted: followTail = false
            onMovementEnded: followTail = atYEnd

            // Centre-of-screen state for an empty conversation: either the
            // model is still loading, or it is ready and waiting for a
            // question. An empty ListView with no explanation is the single
            // most common "is this broken?" moment in a chat UI, and a
            // static caption during a 9-19 second model load reads as a
            // hung window -- a moving indicator is the difference between
            // "working" and "frozen".
            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.spacingM
                visible: messageList.count === 0

                BusyIndicator {
                    running: !AppController.modelReady
                    visible: running
                    implicitWidth: 48
                    implicitHeight: 48
                    Layout.alignment: Qt.AlignHCenter
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    horizontalAlignment: Text.AlignHCenter
                    color: root.palette.placeholderText
                    text: {
                        if (!AppController.modelReady)
                            return qsTr("Loading the local model…\nThis takes a few seconds on first start.")
                        if (AppController.activeCorpusId < 0)
                            return qsTr("Select a group to start chatting.")
                        return qsTr("Ask a question about this group's documents.")
                    }
                }
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

                // Whether the reveal has caught up with the full text.
                //
                // NOT the same thing as `!streaming`, and conflating the
                // two was a real bug: `streaming` is derived from isFresh,
                // which stays true for the entire life of a live answer's
                // delegate, so `!streaming` is permanently false for
                // anything that just arrived. Gating the source
                // disclosure on it meant the disclosure only ever appeared
                // on answers reloaded from history -- never on the answer
                // you just received.
                readonly property bool revealComplete:
                    messageDelegate.revealedWordCount >= messageDelegate.streamWords.length

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

                    // Asymmetric on purpose: the user gets a bubble, the
                    // assistant does not.
                    //
                    // A bubble is a speech affordance -- it says "someone
                    // said this to you", which is right for a short question
                    // and wrong for a long structured answer. Wrapping
                    // markdown in a rounded rectangle fights it: headings,
                    // bullet lists and paragraphs all want the full measure
                    // and a flush left edge, and a bubble that hugs its text
                    // gives them a ragged one. Every assistant of this kind
                    // (Claude, ChatGPT, Gemini) lands on the same split for
                    // the same reason. It also removes the widest surface
                    // that had to be re-measured on every reveal tick.
                    Rectangle {
                        id: userBubble
                        visible: messageDelegate.model.isUser
                        // Hug the text up to 85% of the column, then wrap.
                        // No Behavior on width/height: the user's text never
                        // animates, and an animated resize during the
                        // assistant reveal was visible as juddering.
                        width: Math.min(userLabel.implicitWidth + 2 * Theme.spacingM,
                                        messageColumn.width * 0.85)
                        height: visible ? userLabel.implicitHeight + 2 * Theme.spacingM : 0
                        radius: Theme.radiusL
                        anchors.right: parent.right
                        color: root.palette.accent

                        Label {
                            id: userLabel
                            anchors.fill: parent
                            anchors.margins: Theme.spacingM
                            // PlainText on purpose -- a question that happens
                            // to contain * or _ is text the user typed, not
                            // markup they authored.
                            textFormat: Text.PlainText
                            text: messageDelegate.model.text
                            wrapMode: Text.WordWrap
                            color: root.palette.highlightedText
                        }
                    }

                    // The assistant's answer: full column measure, no
                    // container, rendered as Markdown.
                    Label {
                        id: answerLabel
                        visible: !messageDelegate.model.isUser
                        width: messageColumn.width
                        height: visible ? implicitHeight : 0
                        textFormat: Text.MarkdownText
                        // Once the reveal is complete, show the stored text
                        // verbatim rather than the trimmed prefix.
                        // markdownSafePrefix() must not touch the final
                        // text: if an answer legitimately contains an odd
                        // number of "**", trimming would hide its tail
                        // permanently instead of for a frame.
                        text: messageDelegate.revealComplete
                              ? messageDelegate.model.text
                              : root.markdownSafePrefix(
                                    messageDelegate.streamWords.slice(
                                        0, messageDelegate.revealedWordCount).join(" "))
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSizeAnswer
                        color: root.palette.text
                        // Markdown can produce real links. Without this they
                        // render as links and do nothing.
                        onLinkActivated: link => Qt.openUrlExternally(link)
                    }

                    // Provenance disclosure. Replaces the old row of
                    // filename/chunk-id chips, which showed identifiers
                    // and nothing else: a chunk id tells you which
                    // passage was retrieved but not what it said, so it
                    // could not answer the only question worth asking of
                    // a citation -- is the answer actually supported by
                    // it.
                    //
                    // Shown only when something was actually retrieved.
                    // An answer with no sources -- the CHAT path, or a
                    // search that matched nothing -- gets no affordance
                    // at all rather than a "no source" label: a control
                    // whose only message is that it has nothing to show
                    // is noise on every conversational reply.
                    //
                    // Also waits for the reveal to finish, matching how a
                    // real streaming answer shows citations after the text
                    // rather than mid-reveal.
                    Button {
                        flat: true
                        visible: !messageDelegate.model.isUser && messageDelegate.revealComplete
                                 && messageDelegate.model.sources.length > 0
                        text: qsTr("Source · %1  ⌄").arg(messageDelegate.model.sources.length)
                        font.pixelSize: Theme.fontSizeCaption
                        onClicked: root.openSourceInspector(messageDelegate.model)
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
            visible: !root.groupIngesting
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
                    enabled: AppController.activeCorpusId >= 0 && AppController.modelReady
                             && !AppController.chatBusy
                    placeholderText: qsTr("Ask a question…")
                    onAccepted: if (sendButton.enabled) sendButton.clicked()
                }

                Button {
                    id: sendButton
                    // U+21B5, the return-key glyph -- a text-font
                    // character, not an emoji codepoint (same reasoning
                    // as the sidebar's "‹" back glyph: emoji render as
                    // color stickers against flat chrome).
                    text: "↵"
                    font.pixelSize: 18
                    implicitWidth: 44
                    highlighted: true
                    // White via a custom contentItem -- FluentWinUI3
                    // ignores palette.buttonText on highlighted buttons,
                    // same fix as "+ New Group". The background is the
                    // style's own (NO override): that is what makes this
                    // button pixel-identical to every other accent
                    // button, hover and press states included. Earlier
                    // hand-painted backgrounds all mismatched one state
                    // or another.
                    contentItem: Text {
                        text: sendButton.text
                        font: sendButton.font
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    // Deliberately NOT gated on the field having text: a
                    // disabled control swaps to the palette's disabled
                    // colors (gray accent), which read as broken here.
                    // The button stays live-blue whenever chat itself is
                    // usable; clicking with an empty field is a no-op
                    // (sendChatMessage drops empty messages).
                    enabled: AppController.activeCorpusId >= 0 && AppController.modelReady
                             && !AppController.chatBusy
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Send (Enter)")
                    onClicked: {
                        if (questionField.text.trim().length === 0) {
                            return
                        }
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

    // Source inspector: what produced this answer, and what it read.
    //
    // Deliberately near-fullscreen. Passage text is the point of this
    // dialog, and passages run to hundreds of words -- a modestly sized
    // dialog would turn the one thing worth reading into a scroll-race
    // through a letterbox.
    Dialog {
        id: sourceInspector
        title: qsTr("Source")
        modal: true
        // parent, not just anchors.centerIn: the size is expressed against
        // the overlay, so the overlay has to be this popup's parent for
        // parent.width/height to mean the window. Same reasoning as the
        // history drawer above.
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: parent ? Math.round(parent.width * 0.92) : 0
        height: parent ? Math.round(parent.height * 0.9) : 0
        standardButtons: Dialog.Close

        ScrollView {
            id: inspectorScroll
            anchors.fill: parent
            // Without this the Flickable's contentWidth is unset and the
            // view scrolls sideways as well as vertically.
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
                    text: root.toolHeadline(root.inspectTool)
                    font.weight: Theme.fontWeightBold
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: sourceInspector.palette.placeholderText
                    text: root.toolDetail(root.inspectTool)
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
                        text: root.inspectAnswer
                        textFormat: Text.MarkdownText
                        wrapMode: Text.WordWrap
                        onLinkActivated: link => Qt.openUrlExternally(link)
                    }
                }

                // -- How it searched --
                Label {
                    visible: root.inspectSearchTerms.length > 0
                    Layout.topMargin: Theme.spacingM
                    text: qsTr("SEARCH QUERY")
                    font.pixelSize: Theme.fontSizeCaption
                    font.letterSpacing: 0.6
                    color: sourceInspector.palette.placeholderText
                }

                Rectangle {
                    visible: root.inspectSearchTerms.length > 0
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
                            visible: root.inspectSearchQuery.length > 0
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Rewritten question: %1").arg(root.inspectSearchQuery)
                            color: sourceInspector.palette.placeholderText
                        }

                        // The literal lexical query -- machine vocabulary,
                        // so monospace, not prose styling.
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.inspectSearchTerms
                            font.family: "Menlo"
                            font.pixelSize: Theme.fontSizeCaption
                        }
                    }
                }

                // -- What it read --
                Label {
                    visible: root.inspectSources.length > 0
                    Layout.topMargin: Theme.spacingM
                    text: {
                        if (root.inspectTool === "summary")
                            return qsTr("SUMMARY AND COVERAGE · %1").arg(root.inspectSources.length)
                        if (root.inspectTool === "read")
                            return qsTr("DOCUMENTS AVAILABLE · %1").arg(root.inspectSources.length)
                        return qsTr("RETRIEVED PASSAGES · %1").arg(root.inspectSources.length)
                    }
                    font.pixelSize: Theme.fontSizeCaption
                    font.letterSpacing: 0.6
                    color: sourceInspector.palette.placeholderText
                }

                Repeater {
                    model: root.inspectSources

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

                            // Identity line. chunkId is only present on
                            // SEARCH sources (a specific matched passage);
                            // READ sources are whole documents, with no
                            // single chunk to point at.
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

                            // Retrieval metadata, shown only when present
                            // so READ rows don't display empty fields.
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

                            // The passage itself -- the reason this dialog
                            // exists. PlainText, not Markdown: this is
                            // source material quoted verbatim, and any
                            // punctuation in it must not be reinterpreted
                            // as formatting.
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

                // Older search answers were stored before passage text was
                // persisted, so they list chunk ids with nothing to show.
                // Saying so beats rendering a column of empty cards.
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacingS
                    visible: root.inspectTool === "search" && root.inspectSources.length > 0
                             && root.inspectSources[0].text === undefined
                    wrapMode: Text.WordWrap
                    color: sourceInspector.palette.placeholderText
                    text: qsTr("This answer predates passage text being stored, so only the identifiers above are available for it. New answers include the full passage.")
                }
            }
        }
    }
}
