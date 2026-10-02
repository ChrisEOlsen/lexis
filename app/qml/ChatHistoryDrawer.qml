// Chat history drawer (right edge) + delete-confirm dialog.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Drawer {
    id: historyDrawer
    edge: Qt.RightEdge
    // ChatPanel width, for the 80% cap (the drawer itself parents to the Overlay).
    property real panelWidth: 800

    property int pendingDeleteSessionId: -1
    property string pendingDeleteTitle: ""

    // Relative timestamp ("3 minutes ago" ... "Yesterday"), plain date beyond a week.
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

    // Both dimensions explicit (a Drawer sizes from content; with no height this
    // collapsed to 26px and squeezed the list to 0).
    width: Math.min(340, historyDrawer.panelWidth * 0.8)
    height: parent ? parent.height : 0
    // Parented to the Overlay so a popup declared in a layout renders above all.
    parent: Overlay.overlay

    // Named edge paddings: the style's own edge paddings win over grouped `padding`.
    topPadding: Theme.spacingM
    bottomPadding: Theme.spacingM
    leftPadding: Theme.spacingM
    rightPadding: Theme.spacingM

    // The style doesn't implement Drawer (Basic fallback), so the surface is
    // stated explicitly -- palette-derived, so it still matches.
    background: Rectangle {
        color: Qt.lighter(historyDrawer.palette.window, Theme.layerCard)
        border.width: 1
        border.color: historyDrawer.palette.midlight
    }

    // No explicit size here: the Popup resizes an assigned contentItem (only the
    // Drawer's own `height` above is genuinely required).
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
                            text: historyDrawer.relativeTime(historyDelegate.model.createdAt)
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
                        // Consumes the press: opening the menu must not also resume the session.
                        onClicked: sessionMenu.popup()

                        Menu {
                            id: sessionMenu
                            MenuItem {
                                text: qsTr("Delete chat…")
                                onTriggered: {
                                    historyDrawer.pendingDeleteSessionId = historyDelegate.model.sessionId
                                    historyDrawer.pendingDeleteTitle = historyDelegate.model.title
                                    deleteSessionConfirm.open()
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Stock Dialog (the style implements DialogButtonBox).
    Dialog {
        id: deleteSessionConfirm
        title: qsTr("Delete chat")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Cancel | Dialog.Discard
        onDiscarded: {
            AppController.deleteChatSession(historyDrawer.pendingDeleteSessionId)
            deleteSessionConfirm.close()
        }

        Label {
            width: deleteSessionConfirm.availableWidth
            wrapMode: Text.WordWrap
            text: qsTr("Delete \"%1\"? This cannot be undone.").arg(historyDrawer.pendingDeleteTitle)
        }
    }
}
