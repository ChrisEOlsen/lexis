// Left-hand panel: a single drill-down StackView. Level 1
// (GroupsListView) is every group; clicking one selects it and pushes
// level 2 (GroupDocumentsView), that group's documents; "back" pops.
// Neither level knows about this StackView -- they only emit
// groupOpened()/backRequested().
//
// The panel is a Fluent "layer": a rounded card filled one step lighter
// than the window, with a hairline border. Fluent builds depth by
// stacking lighter fills rather than by drawing shadows, so this is the
// idiomatic way to separate the source list from the content pane -- and
// it replaces the previous 1px divider that only worked while the window
// color happened to contrast with both sides.
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
        // StackView does not clip by default -- during a push/pop the
        // outgoing item is translated aside while the incoming one slides
        // in, and without clipping both render outside this card's bounds
        // mid-transition.
        clip: true

        // Explicit transitions instead of StackView's defaults, which are
        // a 400ms full-width x-slide. At 400ms both pages overlap for
        // most of the animation, which in a narrow column reads as the
        // outgoing page's button smearing across the incoming one. A
        // short offset plus a crossfade keeps the "went deeper / came
        // back" cue without the long two-pages-visible window.
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
