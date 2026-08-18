// Drill-down level 1: every group (corpus), via AppController.corpusModel.
// Clicking a row selects that group and asks the StackView (in
// GroupSidebar.qml) to push GroupDocumentsView -- this component only
// emits `groupOpened`, it doesn't know about the StackView itself.
//
// Rows are stock ItemDelegates. Selection is `highlighted`, which under
// FluentWinUI3 draws the blue left accent bar plus a subtle fill -- the
// standard Fluent navigation-item treatment, and the reason this file no
// longer contains any selected/pressed color logic of its own.
//
// Per-row actions live behind a "..." overflow menu rather than an
// always-visible destructive button. A bare red X on every row shouts
// "delete" at the user permanently, for an action taken approximately
// never; the menu keeps the affordance one click away and lets more row
// actions be added later without redesigning the row.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Item {
    id: root

    signal groupOpened()

    property int pendingDeleteId: -1
    property string pendingDeleteName: ""

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingXS

        Label {
            text: "GROUPS"
            font.pixelSize: Theme.fontSizeCaption
            font.letterSpacing: 0.6
            color: root.palette.placeholderText
            Layout.leftMargin: Theme.spacingS
            Layout.bottomMargin: Theme.spacingXS
        }

        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: AppController.corpusModel
            // Delegate reuse is off deliberately: these lists are a
            // handful of rows, so reuse buys nothing, and a reused
            // delegate briefly showing the previous row's state is a
            // class of flicker not worth risking for no gain.
            reuseItems: false

            delegate: ItemDelegate {
                id: itemDelegate
                required property var model
                width: listView.width
                highlighted: itemDelegate.model.corpusId === AppController.activeCorpusId
                onClicked: {
                    AppController.selectGroup(itemDelegate.model.corpusId)
                    root.groupOpened()
                }

                // Custom contentItem for one reason only: the style's
                // stock contentItem is an IconLabel, which does not
                // elide, and group names are user-supplied. The stock
                // `background` is untouched, so the accent bar and press
                // feedback remain the style's.
                contentItem: RowLayout {
                    spacing: Theme.spacingXS

                    Label {
                        text: itemDelegate.model.displayName
                        color: itemDelegate.palette.text
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }

                    ToolButton {
                        text: "⋯"
                        font.pixelSize: 16
                        implicitWidth: 28
                        implicitHeight: 28
                        // The menu is opened explicitly rather than via
                        // ItemDelegate's own click, and this button
                        // consumes the press, so opening the menu does
                        // not also select the group.
                        onClicked: rowMenu.popup()

                        Menu {
                            id: rowMenu
                            MenuItem {
                                text: qsTr("Delete group…")
                                onTriggered: {
                                    root.pendingDeleteId = itemDelegate.model.corpusId
                                    root.pendingDeleteName = itemDelegate.model.displayName
                                    deleteConfirm.open()
                                }
                            }
                        }
                    }
                }
            }
        }

        Button {
            id: newGroupButton
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingXS
            text: qsTr("+  New Group")
            // highlighted is Fluent's accent (primary action) button.
            highlighted: true
            // White regardless of theme -- the accent fill is dark enough
            // in both light and dark modes for white to read. A custom
            // contentItem, because FluentWinUI3 ignores palette.buttonText
            // on highlighted buttons (verified: the text stayed black).
            contentItem: Text {
                text: newGroupButton.text
                font: newGroupButton.font
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            onClicked: {
                nameField.text = ""
                newGroupDialog.open()
            }
        }
    }

    // Both dialogs are stock: no background override, no hand-built
    // footer button. DialogButtonBox is one of the 49 controls the style
    // implements, so Ok/Cancel come out correct, keyboard-navigable, and
    // consistent with the rest of the app for free.
    Dialog {
        id: deleteConfirm
        title: qsTr("Delete group")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Cancel | Dialog.Discard
        onDiscarded: {
            AppController.deleteGroup(root.pendingDeleteId)
            deleteConfirm.close()
        }

        // Names the chat history explicitly. "everything in it" is
        // accurate but reads as "the documents" -- and the chats really do
        // go: public.chat_sessions.corpus_id references public.corpora
        // ON DELETE CASCADE, and chat_messages cascades from the sessions,
        // so deleting the group's registry row takes every conversation
        // and every message with it in the same transaction. There is no
        // soft delete and no export, so this dialog is the only warning
        // the user gets.
        ColumnLayout {
            width: deleteConfirm.availableWidth
            spacing: Theme.spacingS

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Delete \"%1\"?").arg(root.pendingDeleteName)
                font.weight: Theme.fontWeightBold
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("This permanently deletes the group's documents and its entire chat history — every conversation in this group, not just the current one. This cannot be undone.")
            }
        }
    }

    Dialog {
        id: newGroupDialog
        title: qsTr("New group")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Cancel | Dialog.Ok
        // Ok is disabled until the name is non-empty -- the guard used to
        // live on a hand-built Create button.
        onAccepted: AppController.createGroup(nameField.text)
        Component.onCompleted: okButton.enabled = false

        readonly property var okButton: newGroupDialog.standardButton(Dialog.Ok)

        ColumnLayout {
            width: newGroupDialog.availableWidth
            spacing: Theme.spacingS

            Label { text: qsTr("Group name") }

            TextField {
                id: nameField
                Layout.fillWidth: true
                Layout.minimumWidth: 260
                onTextChanged: newGroupDialog.okButton.enabled = nameField.text.trim().length > 0
                onAccepted: if (nameField.text.trim().length > 0) newGroupDialog.accept()
            }
        }
    }
}
