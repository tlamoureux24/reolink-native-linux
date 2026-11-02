import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Client (app-level) settings — the official client's top-right gear: language,
// startup and tray, notifications, decoding, where snapshots and recordings go,
// the live-view tour, and updates. Every change applies and saves at once.
Dialog {
    id: dialog
    title: qsTr("Client settings")
    modal: true
    width: 520
    height: Math.min(780, parent ? parent.height - 40 : 780)
    anchors.centerIn: parent
    padding: 0

    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
    header: Text {
        text: dialog.title
        color: Theme.text; font.pixelSize: 15; font.bold: true
        padding: Theme.spacing * 2
    }

    component Section: Text {
        color: Theme.text; font.pixelSize: 13; font.bold: true
        Layout.topMargin: Theme.spacing
    }
    component Hint: Text {
        color: Theme.textMuted; font.pixelSize: 11
        wrapMode: Text.WordWrap; Layout.fillWidth: true
    }
    component SettingLine: RowLayout {
        property string label: ""
        default property alias controls: holder.data
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text { text: parent.label; color: Theme.textMuted; font.pixelSize: 12
               Layout.preferredWidth: 200; wrapMode: Text.WordWrap }
        RowLayout { id: holder; spacing: Theme.spacing; Layout.fillWidth: true }
    }
    component Btn: Rectangle {
        property string label: ""
        signal clicked()
        implicitWidth: btnTxt.implicitWidth + 18; implicitHeight: 28; radius: 4
        color: btnHover.hovered ? Theme.accentDim : Theme.surfaceAlt
        border.color: Theme.border
        Text { id: btnTxt; anchors.centerIn: parent; text: parent.label; color: Theme.text; font.pixelSize: 12 }
        HoverHandler { id: btnHover }
        TapHandler { onTapped: parent.clicked() }
    }

    footer: RowLayout {
        Item { Layout.fillWidth: true }
        Btn {
            Layout.margins: Theme.spacing * 2
            label: qsTr("Close")
            onClicked: dialog.close()
        }
    }

    contentItem: ScrollView {
        contentWidth: availableWidth
        clip: true
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            width: parent.width
            spacing: Theme.spacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spacing * 2
                spacing: Theme.spacing

                // ---- Language ----
                Section { text: qsTr("Language") }
                SettingLine {
                    label: qsTr("Interface language")
                    ThemedComboBox {
                        id: langBox
                        Layout.preferredWidth: 220
                        readonly property var langs: AppSettings.languages
                        model: langs.map(function (l) {
                            return l.code === "" ? qsTr("System default (%1)").arg(AppSettings.systemLanguageName)
                                                 : l.name;
                        })
                        currentIndex: Math.max(0, langs.findIndex(function (l) {
                            return l.code === AppSettings.language;
                        }))
                        onActivated: (i) => AppSettings.language = langs[i].code
                    }
                }

                // ---- Startup and tray ----
                Section { text: qsTr("Startup"); visible: Tray.available }
                SettingLine {
                    visible: Tray.available
                    label: qsTr("Start when I log in")
                    Switch { checked: Tray.startOnLogin; onToggled: Tray.startOnLogin = checked }
                }
                SettingLine {
                    visible: Tray.available
                    label: qsTr("Keep running in the tray when the window is closed")
                    Switch { checked: Tray.closeToTray; onToggled: Tray.closeToTray = checked }
                }
                Hint {
                    visible: Tray.available
                    text: qsTr("Started at login, the client opens hidden in the tray and keeps watching for detections.")
                }

                // ---- Notifications ----
                Section { text: qsTr("Notifications") }
                SettingLine {
                    label: qsTr("Desktop notifications")
                    Switch {
                        checked: AppSettings.desktopNotifications
                        onToggled: AppSettings.desktopNotifications = checked
                    }
                }
                Hint { text: qsTr("Detections and cameras going offline still appear in Events.") }

                // ---- Video ----
                Section { text: qsTr("Video") }
                SettingLine {
                    label: qsTr("Hardware decoding")
                    Switch {
                        checked: AppSettings.hardwareDecoding
                        onToggled: AppSettings.hardwareDecoding = checked
                    }
                }
                Hint { text: qsTr("Uses the graphics card (VAAPI/NVDEC). Applies to streams opened from now on; turn it off if video stays black or corrupted.") }

                // ---- Live view tour ----
                Section { text: qsTr("Live view tour") }
                SettingLine {
                    label: qsTr("Show each camera for")
                    ThemedSpinBox {
                        from: 3; to: 300
                        value: AppSettings.tourSeconds
                        editable: true
                        onValueModified: AppSettings.tourSeconds = value
                    }
                    Text { text: qsTr("seconds"); color: Theme.textMuted; font.pixelSize: 12 }
                }
                Hint { text: qsTr("Start the tour with the ⟳ button in Live View; it shows each camera full size in turn.") }

                // ---- Storage ----
                Section { text: qsTr("Snapshots and recordings") }
                SettingLine {
                    label: qsTr("Save to")
                    Text {
                        Layout.fillWidth: true
                        text: AppSettings.recordingsDir !== "" ? AppSettings.recordingsDir
                                                               : qsTr("Videos/Reolink (default)")
                        color: Theme.text; font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                }
                RowLayout {
                    Layout.leftMargin: 200 + Theme.spacing
                    spacing: Theme.spacing
                    Btn { label: qsTr("Change…"); onClicked: folderDialog.open() }
                    Btn { label: qsTr("Use default"); visible: AppSettings.recordingsDir !== ""
                          onClicked: AppSettings.recordingsDir = "" }
                }

                // ---- Updates ----
                Section { text: qsTr("Updates") }
                SettingLine {
                    label: qsTr("Check for updates at startup")
                    Switch { checked: AppSettings.checkUpdates; onToggled: AppSettings.checkUpdates = checked }
                }
                SettingLine {
                    label: qsTr("Version %1").arg(Updater.currentVersion)
                    Btn { label: qsTr("Check now"); onClicked: Updater.check() }
                    Text {
                        Layout.fillWidth: true
                        color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.WordWrap
                        text: Updater.available ? qsTr("Version %1 is available").arg(Updater.latestVersion)
                            : Updater.failed ? Updater.error : ""
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Folder for snapshots and recordings")
        // A file:// URL, percent-encoded ("Vid%C3%A9os"): turn it into a path.
        onAccepted: AppSettings.recordingsDir =
            decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""))
    }
}
