// Left-hand drill-down: groups list, then the selected group's documents.
// A Fluent "layer" card; levels signal up, they don't know about this StackView.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import Lexis

Rectangle {
    id: root
    color: Qt.lighter(root.palette.window, Theme.layerCard)
    radius: Theme.radiusM
    border.width: 1
    border.color: root.palette.midlight

    StackView {
        id: stackView
        anchors.fill: parent
        anchors.margins: Theme.spacingS
        // Clip: pages slide sideways mid-transition and would render outside the card.
        clip: true

        // Short offset + crossfade instead of the default 400ms full slide (which smears
        // the outgoing page's buttons across the incoming one in a narrow column).
        pushEnter: Transition {
            NumberAnimation { property: "x"; from: 24; to: 0; duration: Theme.durationNav; easing.type: Easing.OutCubic }
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durationNav }
        }
        pushExit: Transition {
            NumberAnimation { property: "x"; from: 0; to: -24; duration: Theme.durationNav; easing.type: Easing.OutCubic }
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durationNav }
        }
        popEnter: Transition {
            NumberAnimation { property: "x"; from: -24; to: 0; duration: Theme.durationNav; easing.type: Easing.OutCubic }
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durationNav }
        }
        popExit: Transition {
            NumberAnimation { property: "x"; from: 0; to: 24; duration: Theme.durationNav; easing.type: Easing.OutCubic }
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durationNav }
        }

        initialItem: GroupsListView {
            onGroupOpened: stackView.push(documentsComponent)
        }

        Component {
            id: documentsComponent
            GroupDocumentsView {
                onBackRequested: stackView.pop()
            }
        }
    }
}
