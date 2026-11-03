import QtQuick
import QtQuick.Controls
import ReolinkApp

// SpinBox in the app's dark theme, with -/+ steppers.
SpinBox {
    component SpinStep: Rectangle {
        property string glyph: ""
        property bool pressed: false
        implicitWidth: 24; implicitHeight: 26; radius: 4
        color: pressed ? Theme.accentDim : Theme.surface
        border.color: Theme.border
        Text { anchors.centerIn: parent; text: parent.glyph; color: Theme.text; font.pixelSize: 13 }
    }
    id: spin
    implicitWidth: 96
    implicitHeight: 26
    editable: true
    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
    contentItem: TextInput {
        text: spin.displayText
        color: Theme.text; font.pixelSize: 12
        horizontalAlignment: Qt.AlignHCenter; verticalAlignment: Qt.AlignVCenter
        readOnly: !spin.editable
        validator: spin.validator
        inputMethodHints: Qt.ImhDigitsOnly
    }
    up.indicator: SpinStep { x: spin.width - width; glyph: "+"; pressed: spin.up.pressed }
    down.indicator: SpinStep { x: 0; glyph: "−"; pressed: spin.down.pressed }
}
