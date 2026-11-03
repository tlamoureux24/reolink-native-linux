import QtQuick
import QtQuick.Controls
import ReolinkApp

// CheckBox in the app's dark theme (Basic draws a white box and dark label).
CheckBox {
    id: chk
    indicator: Rectangle {
        implicitWidth: 18; implicitHeight: 18
        x: chk.leftPadding; y: (chk.height - height) / 2
        radius: 4
        color: chk.checked ? Theme.accent : Theme.surfaceAlt
        border.color: chk.checked ? Theme.accent : Theme.border
        Text { anchors.centerIn: parent; text: "\u2713"; visible: chk.checked
               color: Theme.window; font.pixelSize: 12; font.bold: true }
    }
    contentItem: Text {
        text: chk.text; color: Theme.text; font.pixelSize: 12
        leftPadding: chk.indicator.width + 6; verticalAlignment: Text.AlignVCenter
    }
}
