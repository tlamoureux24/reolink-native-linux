import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Privacy-mask editor: draw up to `maxAreas` rectangles on a snapshot; those
// parts of the picture are blacked out in live view and recordings. The device
// takes each area as { block: {x, y, width, height}, screen: {width, height} }
// — coordinates relative to the given screen size, which we take from the
// snapshot the user drew on.
Dialog {
    id: dlg
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(900, (Overlay.overlay ? Overlay.overlay.width : 900) - 80)
    padding: Theme.spacing * 2

    property int deviceRow: -1
    property int maxAreas: 4
    property bool enabledMask: true
    property var areas: []          // [{x, y, w, h}] in snapshot pixels
    property string snapshotPath: ""
    property size screen: Qt.size(1280, 720)
    property real imgAspect: 16 / 9

    title: qsTr("Privacy masks")
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
    // Themed title (the Basic style's header paints a light bar).
    header: Text {
        text: dlg.title
        color: Theme.text; font.pixelSize: 15; font.bold: true
        padding: Theme.spacing * 2
    }

    function openFor(row, mask, range) {
        deviceRow = row;
        maxAreas = range && range.maxAreas ? range.maxAreas : 4;
        enabledMask = !mask || mask.enable === undefined ? true : mask.enable === 1;
        var list = [];
        var src = mask && mask.area ? mask.area : [];
        for (var i = 0; i < src.length; ++i) {
            var b = src[i].block || {}, sc = src[i].screen || {};
            if (sc.width > 0) screen = Qt.size(sc.width, sc.height);
            list.push({ x: b.x || 0, y: b.y || 0, w: b.width || 0, h: b.height || 0 });
        }
        areas = list;
        snapshotPath = "";
        open();
        Devices.snapshot(row);
    }
    function save() {
        var out = [];
        for (var i = 0; i < areas.length; ++i)
            out.push({ block: { x: Math.round(areas[i].x), y: Math.round(areas[i].y),
                                width: Math.round(areas[i].w), height: Math.round(areas[i].h) },
                       screen: { width: screen.width, height: screen.height } });
        Devices.applySetting(deviceRow, "SetMask", { Mask: { channel: Devices.channelOf(deviceRow),
                                                            enable: enabledMask ? 1 : 0, area: out } });
        close();
    }

    Connections {
        target: Devices
        function onSnapshotSaved(row, path) { if (row === dlg.deviceRow && dlg.visible) dlg.snapshotPath = path; }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            text: qsTr("Drag on the image to cover an area (up to %1). Right-click an area to remove it. Covered areas are blacked out in live view and recordings.")
                      .arg(dlg.maxAreas)
            color: Theme.textMuted; font.pixelSize: 12
        }

        Rectangle {
            id: stage
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width / dlg.imgAspect)
            color: Theme.paneBackground
            border.color: Theme.border
            clip: true
            readonly property real sx: width / dlg.screen.width
            readonly property real sy: height / dlg.screen.height

            Image {
                anchors.fill: parent
                fillMode: Image.Stretch
                cache: false
                source: dlg.snapshotPath ? ("file://" + dlg.snapshotPath) : ""
                onStatusChanged: if (status === Image.Ready && sourceSize.width > 0) {
                    dlg.imgAspect = sourceSize.width / sourceSize.height;
                    // Without saved areas, express new ones in the picture's own size.
                    if (dlg.areas.length === 0)
                        dlg.screen = Qt.size(sourceSize.width, sourceSize.height);
                }
            }
            Text {
                anchors.centerIn: parent
                visible: dlg.snapshotPath === ""
                text: qsTr("Loading camera image…")
                color: Theme.textMuted; font.pixelSize: 12
            }

            Repeater {
                model: dlg.areas
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    x: modelData.x * stage.sx; y: modelData.y * stage.sy
                    width: modelData.w * stage.sx; height: modelData.h * stage.sy
                    color: "#cc000000"
                    border.color: Theme.accent; border.width: 2
                    Text { anchors.centerIn: parent; text: index + 1; color: Theme.text; font.pixelSize: 14 }
                }
            }
            Rectangle {   // rubber band while drawing
                id: band
                visible: false
                color: "#552aa7ff"; border.color: Theme.accent
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                property point start
                onPressed: (m) => {
                    if (m.button === Qt.RightButton) {
                        for (var i = dlg.areas.length - 1; i >= 0; --i) {
                            var a = dlg.areas[i];
                            if (m.x >= a.x * stage.sx && m.x <= (a.x + a.w) * stage.sx
                                && m.y >= a.y * stage.sy && m.y <= (a.y + a.h) * stage.sy) {
                                var l = dlg.areas.slice(); l.splice(i, 1); dlg.areas = l;
                                return;
                            }
                        }
                        return;
                    }
                    if (dlg.areas.length >= dlg.maxAreas) return;
                    start = Qt.point(m.x, m.y);
                    band.x = m.x; band.y = m.y; band.width = 0; band.height = 0; band.visible = true;
                }
                onPositionChanged: (m) => {
                    if (!band.visible) return;
                    var x = Math.max(0, Math.min(m.x, width)), y = Math.max(0, Math.min(m.y, height));
                    band.x = Math.min(start.x, x); band.y = Math.min(start.y, y);
                    band.width = Math.abs(x - start.x); band.height = Math.abs(y - start.y);
                }
                onReleased: {
                    if (!band.visible) return;
                    band.visible = false;
                    if (band.width < 8 || band.height < 8) return; // a click, not a drag
                    var l = dlg.areas.slice();
                    l.push({ x: band.x / stage.sx, y: band.y / stage.sy,
                             w: band.width / stage.sx, h: band.height / stage.sy });
                    dlg.areas = l;
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            component TxtBtn: Rectangle {
                property string text: ""
                property color edge: Theme.border
                signal clicked()
                implicitWidth: lbl.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
                color: h.hovered ? Theme.surfaceAlt : "transparent"
                border.color: edge
                Text { id: lbl; anchors.centerIn: parent; text: parent.text; color: Theme.text; font.pixelSize: 12 }
                HoverHandler { id: h }
                TapHandler { onTapped: parent.clicked() }
            }

            ThemedCheckBox {
                text: qsTr("Masks enabled")
                checked: dlg.enabledMask
                onToggled: dlg.enabledMask = checked
            }
            Text { text: qsTr("%1 / %2").arg(dlg.areas.length).arg(dlg.maxAreas)
                   color: Theme.textMuted; font.pixelSize: 12 }
            TxtBtn { text: qsTr("Clear all"); onClicked: dlg.areas = [] }
            Item { Layout.fillWidth: true }
            TxtBtn { text: qsTr("Cancel"); onClicked: dlg.close() }
            TxtBtn { text: qsTr("Save"); edge: Theme.accent; onClicked: dlg.save() }
        }
    }
}
