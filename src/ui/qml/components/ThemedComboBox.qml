import QtQuick
import QtQuick.Controls
import ReolinkApp

// ComboBox in the app's dark theme.
ComboBox {
    id: cmb
    implicitWidth: 80; implicitHeight: 26
    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
    contentItem: Text {
        text: cmb.displayText; color: Theme.text; font.pixelSize: 12
        leftPadding: 8; verticalAlignment: Text.AlignVCenter
    }
    indicator: Text {
        x: cmb.width - width - 8; y: (cmb.height - height) / 2
        text: "\u25be"; color: Theme.textMuted; font.pixelSize: 11
    }
    popup.background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: 4 }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: cmb.width
        contentItem: Text { text: modelData; color: Theme.text; font.pixelSize: 12 }
        background: Rectangle { color: highlighted ? Theme.accentDim : "transparent" }
        highlighted: cmb.highlightedIndex === index
    }
}
