// Drill-down level 1: every group. Clicking selects and emits `groupOpened`
// (the StackView push lives in GroupSidebar.qml, not here).
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
            // No delegate reuse: a handful of rows gains nothing, and reuse can flicker.
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

                // Custom contentItem only to elide (IconLabel doesn't); background stays stock.
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
                        // Consumes the press: opening the menu must not also select the group.
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
            // White custom contentItem: the style ignores palette.buttonText when highlighted.
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

    // Both dialogs are stock (the style implements DialogButtonBox).
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

        // Names chat history explicitly: it cascades away with the group, no undo.
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
        // Ok stays disabled until the name is non-empty.
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
