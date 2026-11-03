pragma Singleton
import QtQuick
import ReolinkApp.Core

// Dark and light palettes approximating the official Reolink Client, chosen by
// AppSettings.theme ("dark", "light", or "system" to follow the desktop).
// Single source of truth for colors/metrics — no literal colors anywhere else
// in the UI, except overlays drawn on top of video, which stays dark.
QtObject {
    readonly property bool dark: AppSettings.theme === "system"
                                 ? Qt.styleHints.colorScheme !== Qt.ColorScheme.Light
                                 : AppSettings.theme !== "light"

    readonly property color window: dark ? "#10161d" : "#eef1f4"
    readonly property color surface: dark ? "#1a222b" : "#ffffff"
    readonly property color surfaceAlt: dark ? "#222d38" : "#e4e9ee"
    readonly property color border: dark ? "#2c3945" : "#cbd4dc"
    readonly property color accent: dark ? "#2aa7ff" : "#1580cc"
    readonly property color accentDim: dark ? "#1d6faa" : "#b9dcf5"
    readonly property color text: dark ? "#e8eef4" : "#17212b"
    readonly property color textMuted: dark ? "#8b98a5" : "#5b6874"
    // Behind video: dark in both themes, as the picture's letterboxing is.
    readonly property color paneBackground: "#05080b"
    readonly property color danger: dark ? "#e5484d" : "#d9363c"
    readonly property color online: dark ? "#46a758" : "#2f8f45"
    // A barely-there edge, and a translucent tint for buttons on coloured bars.
    readonly property color faintBorder: dark ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(0, 0, 0, 0.05)
    readonly property color tint: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06)
    readonly property color tintHover: dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.14)
    // Set and unset cells of a schedule grid.
    readonly property color cellOn: dark ? "#1d6faa" : "#1580cc"
    readonly property color cellOff: dark ? "#151d26" : "#dde3e9"

    readonly property int radius: 6
    readonly property int spacing: 8
    // How far the pointer must move before a press becomes a drag rather than
    // a click, for dragging cameras between grid cells.
    readonly property int dragThreshold: 8
    readonly property int sidebarWidth: 240
    readonly property int navHeight: 48
}
