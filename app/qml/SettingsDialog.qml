// Settings: the two knobs users actually reach for, live-applied --
// thinking (slower, more careful answers) and the meaning-based
// reranker -- plus read-only model info. Everything else stays in
// config/lexis.conf (see docs/configuration.md), reachable via the
// "Open config folder" button; the dialog never edits or displays
// db_conninfo.
//
// A gear ToolButton in ChatPanel's header opens this. Stock Dialog
// throughout, same conventions as the other dialogs.
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

        // Both live switches call into the C core: setThinkingEnabled()
        // only changes the value the NEXT QueryWorker is handed, but
        // setRerankerEnabled() calls retrieval_set_reranker_enabled(),
        // which writes retrieval.c's gate flags from this thread and can
        // start a model load. retrieval.h documents that as "never during
        // a retrieval_run()", and only chatBusy tells us that here -- the
        // gear button itself stays enabled so the panel is still readable
        // mid-query.
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
                    // Toggling assigned `checked` directly, which destroyed
                    // the binding above. Restore it: a write that failed
                    // (read-only config, disk full) leaves the controller's
                    // value unchanged, and the switch has to snap back to
                    // it rather than sit there claiming a setting the
                    // engine never got.
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
                // Reveal-only: the finder opens with the config selected;
                // editing stays a deliberate, human act on a file the app
                // promises to share (docs/configuration.md).
                Qt.openUrlExternally(AppController.configDirectoryUrl())
            }
        }
    }
}