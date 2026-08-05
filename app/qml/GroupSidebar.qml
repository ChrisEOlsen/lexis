// Left-hand "groups" panel: lists every corpus via AppController.corpusModel,
// lets the user create/delete groups and pick which one is active.
//
// ComponentBehavior: Bound -- delegates below reference IDs declared in
// this outer file (listView, root, deleteConfirm). Without this pragma
// those lookups happen dynamically at runtime and qmllint flags every
// one as "unqualified access"; with it they bind lexically at compile
// time, which is both faster and statically checkable.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Rectangle {
    id: root
    color: "#f5f5f5"

    property int pendingDeleteId: -1
    property string pendingDeleteName: ""

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: AppController.corpusModel

            delegate: ItemDelegate {
                id: itemDelegate
                required property var model
                width: listView.width
                highlighted: model.corpusId === AppController.activeCorpusId
                onClicked: AppController.selectGroup(model.corpusId)

                contentItem: RowLayout {
                    Label {
                        text: itemDelegate.model.displayName
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolButton {
                        text: "✕"
                        onClicked: {
                            root.pendingDeleteId = itemDelegate.model.corpusId
                            root.pendingDeleteName = itemDelegate.model.displayName
                            deleteConfirm.open()
                        }
                    }
                }
            }
        }

        Button {
            Layout.fillWidth: true
            text: "New Group"
            onClicked: {
                nameField.text = ""
                newGroupDialog.open()
            }
        }
    }

    Dialog {
        id: deleteConfirm
        title: "Delete Group"
        standardButtons: Dialog.Yes | Dialog.No
        modal: true
        anchors.centerIn: parent

        Label {
            text: "Delete \"" + root.pendingDeleteName + "\" and everything in it? This cannot be undone."
            wrapMode: Text.WordWrap
        }
        onAccepted: AppController.deleteGroup(root.pendingDeleteId)
    }

    Dialog {
        id: newGroupDialog
        title: "New Group"
        standardButtons: Dialog.Ok | Dialog.Cancel
        modal: true
        anchors.centerIn: parent

        ColumnLayout {
            Label {
                text: "Group name:"
            }
            TextField {
                id: nameField
                Layout.fillWidth: true
                Layout.minimumWidth: 220
                onAccepted: newGroupDialog.accept()
            }
        }
        onAccepted: {
            if (nameField.text.trim().length > 0) {
                AppController.createGroup(nameField.text)
            }
        }
    }
}
