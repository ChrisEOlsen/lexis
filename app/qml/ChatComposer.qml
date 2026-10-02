// Question input bar: stock TextField + accent Button, on the conversation measure.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Item {
    id: composer
    property int columnWidth: 760

    implicitHeight: composerRow.implicitHeight

    RowLayout {
        id: composerRow
        width: Math.min(parent.width, composer.columnWidth)
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
            // U+21B5 return-key glyph: text font, not emoji (color stickers).
            text: "↵"
            font.pixelSize: 18
            implicitWidth: 44
            highlighted: true
            // White custom contentItem (style ignores buttonText when highlighted);
            // background stays stock so states match every other accent button.
            contentItem: Text {
                text: sendButton.text
                font: sendButton.font
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            // Not gated on the field having text (a greyed button reads as broken);
            // clicking with an empty field is a no-op.
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
