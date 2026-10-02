// Settings: thinking + reranker switches (live-applied) and read-only model info.
// Everything else stays in config/lexis.conf via "Open config folder".
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lexis

Dialog {
    id: settings

    title: qsTr("Settings")
    modal: true
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Ok
    width: 460

    contentItem: ColumnLayout {
        spacing: Theme.spacingL

        // Switches are disabled mid-query: the reranker gate must not change during a run.
        Label {
            visible: AppController.chatBusy
            text: qsTr("These can't change while a question is being answered.")
            color: settings.palette.placeholderText
            font.pixelSize: Theme.fontSizeCaption
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // -- Deeper thinking --
        RowLayout {
            spacing: Theme.spacingM
            Layout.fillWidth: true

            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                Label {
                    text: qsTr("Deeper thinking")
                    font.weight: Theme.fontWeightBold
                }

                Label {
                    text: qsTr("Run a reasoning pass before answering. Slower (about 3x), more careful.")
                    color: settings.palette.placeholderText
                    font.pixelSize: Theme.fontSizeCaption
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            Switch {
                enabled: !AppController.chatBusy
                checked: AppController.thinkingEnabled
                onToggled: {
                    AppController.setThinkingEnabled(checked)
                    // Restore the destroyed binding so a failed write snaps back.
                    checked = Qt.binding(function() { return AppController.thinkingEnabled })
                }
            }
        }

        // -- Meaning-based reranking --
        RowLayout {
            spacing: Theme.spacingM
            Layout.fillWidth: true

            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                Label {
                    text: qsTr("Meaning-based reranking")
                    font.weight: Theme.fontWeightBold
                }

                Label {
                    text: qsTr("Re-order search matches by meaning as well as keywords. Slightly slower per question.")
                    color: settings.palette.placeholderText
                    font.pixelSize: Theme.fontSizeCaption
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            Switch {
                enabled: !AppController.chatBusy
                checked: AppController.rerankerEnabled
                onToggled: {
                    AppController.setRerankerEnabled(checked)
                    checked = Qt.binding(function() { return AppController.rerankerEnabled })
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: settings.palette.midlight
        }

        // -- Model (read-only) --
        ColumnLayout {
            spacing: Theme.spacingXS
            Layout.fillWidth: true

            Label {
                text: qsTr("CHAT MODEL")
                font.pixelSize: Theme.fontSizeCaption
                font.letterSpacing: 0.6
                color: settings.palette.placeholderText
            }

            Label {
                text: AppController.modelDisplayName.length > 0
                      ? AppController.modelDisplayName
                      : qsTr("(from config/lexis.conf)")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Changing the model is a config-file edit and takes effect on the next launch.")
                color: settings.palette.placeholderText
                font.pixelSize: Theme.fontSizeCaption
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        // -- Everything else: the file --
        Button {
            flat: true
            text: qsTr("Open config folder")
            onClicked: {
                // Reveal-only: editing the shared config stays a deliberate human act.
                Qt.openUrlExternally(AppController.configDirectoryUrl())
            }
        }
    }
}