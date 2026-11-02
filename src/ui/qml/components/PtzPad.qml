import QtQuick
import ReolinkApp
import ReolinkApp.Core

// PTZ joystick: 8-way directional pad (press-and-hold to move, release to Stop)
// plus zoom and focus rockers, and a "⋯" button that opens the PtzPanel
// (speed, positions, presets, patrol, guard point, auto-tracking). Sends
// PtzCtrl via the Devices model.
Rectangle {
    id: pad
    property int deviceRow: -1
    property bool showZoom: true
    // Pan/tilt-capable (ptzType); zoom-only cameras such as the RLC-811A get
    // no arrows that would do nothing.
    readonly property var info: deviceRow >= 0 ? Devices.cameraInfo(deviceRow) : ({})
    readonly property bool showDirections: info.capPanTilt !== undefined ? info.capPanTilt : true
    readonly property bool showFocus: showZoom && !!info.capFocus
    readonly property bool hasPanel: !!(info.capPtzSpeed || info.capZoomFocusPos || info.capAutoFocus
                                        || info.capPtzPreset || info.capPtzPatrol || info.capPtzGuard
                                        || info.capAutoTrack)
    property int speed: 32

    width: 120
    height: column.implicitHeight + 12
    radius: Theme.radius
    color: "#e60d141b"
    border.color: Theme.border

    function move(op) { if (deviceRow >= 0) Devices.ptzMove(deviceRow, op, pad.speed); }
    function stop() { if (deviceRow >= 0) Devices.ptzStop(deviceRow); }
    function openPanel() { panel.open(); }

    component Rocker: Rectangle {
        property string op: ""
        property string glyph: ""
        width: 46; height: 30; radius: 6
        color: rpress.pressed ? Theme.accentDim : Theme.surface
        border.color: Theme.border
        Text { anchors.centerIn: parent; text: parent.glyph; color: Theme.text; font.pixelSize: 15 }
        MouseArea {
            id: rpress
            anchors.fill: parent
            onPressed: pad.move(parent.op)
            onReleased: pad.stop()
            onCanceled: pad.stop()
        }
    }

    Column {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 6
        spacing: 6

        // Directional ring
        Item {
            visible: pad.showDirections
            width: 104; height: 104
            anchors.horizontalCenter: parent.horizontalCenter

            Rectangle { // hub
                anchors.centerIn: parent
                width: 34; height: 34; radius: 17
                color: Theme.surfaceAlt
                border.color: Theme.border
            }

            component Dir: Rectangle {
                property string op: ""
                property string glyph: ""
                width: 30; height: 30; radius: 6
                color: dpress.pressed ? Theme.accentDim : Theme.surface
                border.color: Theme.border
                Text { anchors.centerIn: parent; text: parent.glyph; color: Theme.text; font.pixelSize: 13 }
                MouseArea {
                    id: dpress
                    anchors.fill: parent
                    onPressed: pad.move(parent.op)
                    onReleased: pad.stop()
                    onCanceled: pad.stop()
                }
            }

            Dir { op: "Up";        glyph: "▲"; anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter }
            Dir { op: "Down";      glyph: "▼"; anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter }
            Dir { op: "Left";      glyph: "◀"; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
            Dir { op: "Right";     glyph: "▶"; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter }
            Dir { op: "LeftUp";    glyph: "↖"; anchors.top: parent.top; anchors.left: parent.left }
            Dir { op: "RightUp";   glyph: "↗"; anchors.top: parent.top; anchors.right: parent.right }
            Dir { op: "LeftDown";  glyph: "↙"; anchors.bottom: parent.bottom; anchors.left: parent.left }
            Dir { op: "RightDown"; glyph: "↘"; anchors.bottom: parent.bottom; anchors.right: parent.right }
        }

        // Zoom rocker
        Row {
            visible: pad.showZoom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Rocker { op: "ZoomDec"; glyph: "−" }
            Rocker { op: "ZoomInc"; glyph: "+" }
        }

        // Focus rocker (near / far)
        Row {
            visible: pad.showFocus
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Rocker { op: "FocusDec"; glyph: "◐−" }
            Rocker { op: "FocusInc"; glyph: "◐+" }
        }

        // Advanced panel
        Rectangle {
            visible: pad.hasPanel
            width: 100; height: 26; radius: 6
            anchors.horizontalCenter: parent.horizontalCenter
            color: moreHover.hovered || panel.opened ? Theme.accentDim : Theme.surface
            border.color: Theme.border
            Text { anchors.centerIn: parent; text: "⋯ " + qsTr("More"); color: Theme.text; font.pixelSize: 12 }
            HoverHandler { id: moreHover }
            TapHandler { onTapped: panel.opened ? panel.close() : panel.open() }
        }
    }

    PtzPanel {
        id: panel
        deviceRow: pad.deviceRow
        speed: pad.speed
        onSpeedPicked: (value) => pad.speed = value
        // Open to the left of the pad (it sits on the pane's right edge);
        // Popup margins keep it inside the window.
        x: -width - 8
        y: pad.height - height
    }
}
