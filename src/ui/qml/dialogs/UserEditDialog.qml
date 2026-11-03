import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Add a user, or change an existing user's password, on a host (NVR/camera).
// Shapes verified against reolink_aio: AddUser {User:{userName,password,level}},
// ModifyUser {User:{userName,newPassword,oldPassword}}. Admin-only (the caller
// gates the entry points); the write itself is also rejected server-side if not.
Dialog {
    id: dialog
    modal: true
    width: 420
    anchors.centerIn: Overlay.overlay

    property int deviceRow: -1
    property string mode: "add"        // "add" | "password"
    property string userName: ""       // pre-filled + locked in password mode

    title: mode === "add" ? qsTr("Add user") : qsTr("Change password")
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
    header: Text {
        text: dialog.title
        color: Theme.text; font.pixelSize: 15; font.bold: true
        padding: Theme.spacing * 2
    }
    // Themed buttons rather than the Basic style's light button box.
    footer: RowLayout {
        spacing: Theme.spacing
        Item { Layout.fillWidth: true }
        Rectangle {
            Layout.bottomMargin: Theme.spacing * 2
            implicitWidth: cxTxt.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
            color: cxHover.hovered ? Theme.surfaceAlt : "transparent"
            border.color: Theme.border
            Text { id: cxTxt; anchors.centerIn: parent; text: qsTr("Cancel"); color: Theme.text; font.pixelSize: 12 }
            HoverHandler { id: cxHover }
            TapHandler { onTapped: dialog.reject() }
        }
        Rectangle {
            Layout.bottomMargin: Theme.spacing * 2
            Layout.rightMargin: Theme.spacing * 2
            implicitWidth: okTxt.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
            color: okHover.hovered ? Theme.accent : Theme.accentDim
            border.color: Theme.accent
            Text { id: okTxt; anchors.centerIn: parent; text: qsTr("OK"); color: Theme.text; font.pixelSize: 12 }
            HoverHandler { id: okHover }
            TapHandler { onTapped: dialog.accept() }
        }
    }

    component Field: TextField {
        Layout.fillWidth: true
        color: Theme.text
        placeholderTextColor: Theme.textMuted
        background: Rectangle {
            color: Theme.surfaceAlt
            border.color: parent.activeFocus ? Theme.accent : Theme.border
            radius: 4
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        Field {
            id: nameField
            placeholderText: qsTr("Username")
            text: dialog.userName
            enabled: dialog.mode === "add"
        }
        Field {
            id: oldPassField
            visible: dialog.mode === "password"
            placeholderText: qsTr("Current password")
            echoMode: TextInput.Password
        }
        Field {
            id: passField
            placeholderText: dialog.mode === "add" ? qsTr("Password") : qsTr("New password")
            echoMode: TextInput.Password
        }
        RowLayout {
            visible: dialog.mode === "add"
            Layout.fillWidth: true
            Text { text: qsTr("Privilege"); color: Theme.textMuted; font.pixelSize: 12 }
            ComboBox {
                id: levelBox
                Layout.preferredWidth: 160
                model: ["guest", "admin"]
            }
            Item { Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.mode === "add"
            text: qsTr("Guests can view live/playback; admins can also change settings.")
            color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.WordWrap
        }
    }

    onAccepted: {
        if (dialog.deviceRow < 0)
            return;
        if (dialog.mode === "add") {
            if (nameField.text.length === 0 || passField.text.length === 0)
                return;
            Devices.applySetting(dialog.deviceRow, "AddUser",
                { "User": { "userName": nameField.text, "password": passField.text,
                            "level": levelBox.currentText } });
        } else {
            if (passField.text.length === 0)
                return;
            Devices.applySetting(dialog.deviceRow, "ModifyUser",
                { "User": { "userName": dialog.userName, "newPassword": passField.text,
                            "oldPassword": oldPassField.text } });
        }
    }

    onClosed: { passField.text = ""; oldPassField.text = ""; nameField.text = ""; }
}
