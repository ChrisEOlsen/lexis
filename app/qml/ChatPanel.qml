// Chat panel: header, message list, composer, history drawer, source inspector.
// Stock controls + palette-derived bubbles; root is a plain Item (Popups must not be layout children).
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Item {
    id: root
    // Root stays enabled (disabling it would grey the startup loading indicator);
    // controls that need a group gate themselves individually.

    // Caps the conversation column at a readable measure, centered.
    readonly property int columnWidth: 760

    // True when the viewed group is the one ingesting (chat replaced by progress).
    readonly property bool groupIngesting: AppController.activeCorpusId >= 0
                                           && AppController.activeCorpusId === AppController.ingestingCorpusId

    // Main.qml owns the collapsed state; this only requests the flip.
    signal sidebarToggleRequested()

    DocumentViewerDialog {
        id: documentViewer
    }

    SettingsDialog {
        id: settingsDialog
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingS

        // -- Header --
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingXS

            // Sidebar toggle (state owned by Main.qml). Fixed square, glyph dead-centre.
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
                    // "Select a group", not "LEXIS" (the window frame already says that).
                    text: AppController.activeCorpusId >= 0 ? AppController.activeCorpusName : qsTr("Select a group")
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.fontWeightBold
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Label {
                    // No model-loading caption here: the loading state owns the conversation area.
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
                // Custom contentItem: glyph larger than label, both centred. Text glyphs
                // (not an icon font), default palette colours (track enabled state).
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

            // Settings (always enabled: useful before any group exists).
            Button {
                id: settingsButton
                flat: true
                // Square single-glyph button; vertical padding keeps History/New chat height.
                leftPadding: 0
                rightPadding: 0
                implicitWidth: 38
                onClicked: settingsDialog.open()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Settings")
                contentItem: Label {
                    text: "⚙"
                    font.pixelSize: 18
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Usage help (always enabled: useful before any group exists).
            Button {
                id: helpButton
                flat: true
                leftPadding: 0
                rightPadding: 0
                implicitWidth: 38
                onClicked: helpDialog.open()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("How to get the best answers")
                contentItem: Label {
                    text: "?"
                    font.pixelSize: 18
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
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
                // Duration from C++: short steps while reading, one long glide through the
                // hook-less index rebuild's estimate (see IngestWorker.h).
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
                // Lossless: cancelling leaves the group as it was (see cancelIngest()).
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

            // Keeps the newest message in view (deferred: delegate heights settle after
            // countChanged). followTail stops auto-scroll while the user rereads above.
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

            // Centre-of-screen empty state: loading indicator or the waiting prompt.
            // A moving indicator during the ~9-19s load reads as "working", not frozen.
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

            delegate: MessageDelegate {
                columnWidth: root.columnWidth
                isLastMessage: index === messageList.count - 1
                onInspectRequested: messageModel => sourceInspector.openFor(messageModel)
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

                    // The style's own BusyIndicator (not hand-animated dots).
                    BusyIndicator {
                        running: AppController.chatBusy
                        implicitWidth: 20
                        implicitHeight: 20
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Label {
                        // Live stage text (a static "Thinking..." over ~8-20s reads as a hang).
                        // Empty falls back to the placeholder so the footer never blanks.
                        text: AppController.queryStageText.length > 0
                              ? AppController.queryStageText
                              : qsTr("Thinking…")
                        color: root.palette.placeholderText
                        font.pixelSize: Theme.fontSizeCaption
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }

        // -- Composer (stock TextField + accent Button, on the conversation measure) --
        ChatComposer {
            visible: !root.groupIngesting
            Layout.fillWidth: true
            // spacingL: the last line sat nearly flush against the input bar.
            Layout.topMargin: Theme.spacingL
            columnWidth: root.columnWidth
        }
    }

    // -- Chat history --
    ChatHistoryDrawer {
        id: historyDrawer
        panelWidth: root.width
    }

    // Source inspector: what produced this answer, and what it read.
    SourceInspectorDialog {
        id: sourceInspector
        documentViewer: documentViewer
    }

    // -- Usage help ("?" in the header) --
    Dialog {
        id: helpDialog
        title: qsTr("Getting the best out of LEXIS")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Ok
        // Fixed dialog width AND constant label width: a RichText label referencing
        // the dialog's geometry loops implicitHeight (observed at runtime).
        width: 520

        Label {
            width: 470
            wrapMode: Text.WordWrap
            textFormat: Text.RichText
            text: qsTr(
                "<p>LEXIS finds answers by <b>matching the words in your question " +
                "against the words in your documents</b> (plus close synonyms), " +
                "re-ordering the best matches by meaning, and writing an answer " +
                "from those passages only.</p>" +
                "<p><b>Use specific keywords.</b> Because the search is word-based, " +
                "concrete terms that likely appear in the document work best -- " +
                "names, part numbers, error codes, section titles. " +
                "“oil filter torque spec” finds more than " +
                "“how tight should that thing be”.</p>" +
                "<p><b>Follow-ups are fine.</b> LEXIS rewrites them into standalone " +
                "questions using the conversation, but the same rule applies: the " +
                "more concrete words, the better the search.</p>" +
                "<p><b>Broad questions work differently.</b> “What is this " +
                "collection about?” is answered from a generated overview of " +
                "the whole group rather than a search.</p>" +
                "<p><b>Check the Source panel</b> under any answer to see the exact " +
                "search that ran and the passages the answer came from. If an " +
                "answer seems off, that is the place to look.</p>")
        }
    }
}
