import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Device Settings: device selector + category sidebar + panel. Panels fetch their
// Get* commands on show and render editable forms; writes are gated on the
// device's admin flag (disabled with a tooltip otherwise). Mirrors the official
// client's settings structure (DESIGN §6.7).
Item {
    id: page

    property int deviceRow: -1
    property string category: "system"
    // Test hooks (RL_SETTINGS_CATEGORY / RL_SETTINGS_ROW) for --smoke screenshots.
    Component.onCompleted: {
        if (typeof settingsCategory !== "undefined" && settingsCategory !== "")
            category = settingsCategory;
        if (typeof settingsRow !== "undefined" && settingsRow >= 0)
            Qt.callLater(function () { page.showDevice(settingsRow); });
    }
    property var settings: ({})      // cmd -> value map from the last fetch
    property bool isAdmin: false
    function refreshAdmin() {
        page.isAdmin = page.deviceRow >= 0 ? Devices.isAdminAt(page.deviceRow) : false;
    }
    property string status: ""
    // A message kept on screen must follow a live language switch: remember
    // how to build it and rebuild it when the language changes.
    property var _statusFn: null
    function setStatus(fn) {
        _statusFn = fn;
        page.status = fn ? fn() : "";
    }
    Connections {
        target: AppSettings
        function onLanguageChanged() { if (page._statusFn) page.status = page._statusFn(); }
    }

    // Called from the sidebar's Settings action: focus this device row.
    function showDevice(row) {
        if (row >= 0 && row < Devices.count)
            deviceCombo.currentIndex = row;
    }

    readonly property var categories: [
        { key: "image",     label: qsTr("Image"),           cmds: ["GetIsp", "GetIrLights", "GetPowerLed"] },
        { key: "display",   label: qsTr("Display / OSD"),    cmds: ["GetOsd", "GetMask"] },
        { key: "encoding",  label: qsTr("Encoding"),        cmds: ["GetEnc"] },
        { key: "recording", label: qsTr("Recording"),       cmds: [] },
        { key: "detection", label: qsTr("Detection / Alerts"), cmds: ["GetAiCfg"] },
        { key: "lights",    label: qsTr("Lights & siren"),  cmds: ["GetWhiteLed", "GetAudioAlarmV20", "GetAudioCfg"] },
        { key: "network",   label: qsTr("Network"),         cmds: ["GetLocalLink", "GetNetPort"] },
        { key: "storage",   label: qsTr("Storage"),         cmds: ["GetHddInfo"] },
        { key: "users",     label: qsTr("Users"),           cmds: ["GetUser"] },
        { key: "time",      label: qsTr("Time"),            cmds: ["GetTime", "GetNtp"] },
        { key: "system",    label: qsTr("System"),          cmds: ["GetDevInfo", "GetTime", "GetAutoUpgrade"] }
    ]

    readonly property var httpSetters: ["SetIsp", "SetIrLights", "SetPowerLed", "SetMask", "SetWhiteLed",
                                        "SetAudioAlarmV20", "SetAudioCfg", "SetAutoUpgrade"]
    function ch() { return Devices.channelOf(page.deviceRow); }
    // Range block of a Get command ("GetIspRange"), or {} when absent.
    function rng() {
        var o = page.settings[arguments[0] + "Range"];
        for (var i = 1; i < arguments.length && o; i++) o = o[arguments[i]];
        return o === undefined || o === null ? ({}) : o;
    }

    function cmdsFor(key) {
        for (var i = 0; i < categories.length; i++)
            if (categories[i].key === key) return categories[i].cmds;
        return [];
    }
    property bool fetched: false     // a fetch has completed (values may be partial)
    function has(cmd) { return page.settings.hasOwnProperty(cmd); }

    // Alert enables (push/email/ftp) come over native Baichuan, not the flaky
    // HTTP-CGI — keys ok/push/email/ftp, each 0/1 or -1. Empty until loaded.
    property var alerts: ({})

    // Image config over native Baichuan (cmd 26 flat { tag: value } map, values as
    // strings). imgReady flips true once the first fetch returns.
    property var img: ({})
    property bool imgReady: false

    // Per-AI-type detection config over Baichuan (cmd 342), keyed by type
    // (people/vehicle/dog_cat) -> flat map incl. sensitivity/stayTime.
    property var aiSens: ({})
    // Recording config over Baichuan (cmd 54 flat map).
    property var rec: ({})
    property var recSched: ({})   // type -> 168-char weekly table (cmd 81)
    property bool recReady: false
    // Motion detection config over Baichuan (cmd 46 flat map).
    property var md: ({})
    function aiBody(type) {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<AiDetectCfg version=\"1.1\">\n<chn>"
             + Devices.channelOf(page.deviceRow) + "</chn>\n<type>" + type + "</type>\n</AiDetectCfg>\n</body>\n";
    }

    function fetch() {
        page.settings = ({});
        page.fetched = false;
        if (page.deviceRow >= 0) {
            // Baichuan-only categories (Image, Recording) have no HTTP Get*
            // commands — don't fire an empty fetchSettings (it reports "device not
            // ready"); their BC fetches below drive their own loading state.
            var httpCmds = cmdsFor(page.category);
            page.setStatus(function () { return httpCmds.length > 0 ? qsTr("Loading…") : ""; });
            if (httpCmds.length > 0)
                Devices.fetchSettings(page.deviceRow, httpCmds);
            if (page.category === "detection") {
                page.alerts = ({});
                Devices.fetchAlerts(page.deviceRow);
                page.aiSens = ({});
                page.md = ({});
                Devices.fetchBcConfig(page.deviceRow, 46);
                var aiTypes = ["people", "vehicle", "dog_cat"];
                for (var i = 0; i < aiTypes.length; i++)
                    Devices.fetchBcConfig(page.deviceRow, 342, page.aiBody(aiTypes[i]));
            }
            if (page.category === "image") {
                page.img = ({});
                page.imgReady = false;
                Devices.fetchBcConfig(page.deviceRow, 26);
            }
            if (page.category === "recording") {
                page.rec = ({});
                page.recSched = ({});
                page.recReady = false;
                Devices.fetchBcConfig(page.deviceRow, 54);
                Devices.fetchRecSchedule(page.deviceRow);
            }
        } else {
            page.setStatus(function () { return qsTr("Select a device"); });
        }
    }
    // Nested lookup helper: val("GetEnc","Enc","mainStream","size").
    function val() {
        var o = page.settings;
        for (var i = 0; i < arguments.length; i++) {
            if (o === undefined || o === null) return undefined;
            o = o[arguments[i]];
        }
        return o;
    }

    Connections {
        target: Devices
        function onSettingsLoaded(row, values) {
            if (values["_tag"])
                return; // another panel's fetch (e.g. PTZ)
            if (row === page.deviceRow) {
                page.settings = values;
                page.fetched = true;
                var f = values["_failed"] || [];
                page.setStatus(function () { return f.length > 0 ? qsTr("%1 unavailable on this device").arg(f.join(", ")) : ""; });
            }
        }
        function onSettingsFailed(row, error) {
            if (row === page.deviceRow) { page.fetched = true; page.setStatus(function () { return error; }); }
        }
        // A device's rows are re-inserted once it finishes signing in. If this
        // page asked before that, its fetch failed with "device not ready" and
        // nothing retried: fetch again when the shown device has no settings yet.
        function onRowsInserted() {
            page.refreshAdmin();
            if (page.deviceRow >= 0 && Object.keys(page.settings).length === 0)
                Qt.callLater(page.fetch);
        }
        // isAdminAt() isn't a binding: re-read it whenever device data changes
        // (it is only known once the device has signed in).
        function onDataChanged() { page.refreshAdmin(); }
        function onSettingApplied(row, command, ok, error) {
            if (row !== page.deviceRow)
                return;
            page.setStatus(function () { return ok ? qsTr("%1 saved").arg(command) : (command + ": " + error); });
            // Reflect user add/remove/password changes immediately.
            if (ok && (command === "AddUser" || command === "DelUser" || command === "ModifyUser"))
                page.fetch();
            // Re-read alert enables after a Baichuan toggle so the switch reflects
            // the device's confirmed state.
            if (command === "push" || command === "email" || command === "ftp")
                Devices.fetchAlerts(page.deviceRow);
            // HTTP setters of the lot-3 panels: re-read so controls show the
            // device's confirmed values (without clearing the page).
            if (command === "UpgradeOnline" && !ok)
                page.fwUpgrading = false; // refused: nothing is installing
            if (ok && page.httpSetters.indexOf(command) >= 0)
                Devices.fetchSettings(page.deviceRow, page.cmdsFor(page.category));
            if (command === "Set25")   // image write over Baichuan
                Devices.fetchBcConfig(page.deviceRow, 26);
            if (command === "Set55")   // recording write over Baichuan
                Devices.fetchBcConfig(page.deviceRow, 54);
            if (command === "Set47")   // motion write over Baichuan
                Devices.fetchBcConfig(page.deviceRow, 46);
            if (command === "Set343") {   // AI sensitivity write over Baichuan
                var ts = ["people", "vehicle", "dog_cat"];
                for (var i = 0; i < ts.length; i++)
                    Devices.fetchBcConfig(page.deviceRow, 342, page.aiBody(ts[i]));
            }
        }
        function onAlertsLoaded(row, values) {
            if (row === page.deviceRow)
                page.alerts = values;
        }
        function onRecScheduleLoaded(row, values) {
            if (row === page.deviceRow)
                page.recSched = values;
        }
        function onBcConfigLoaded(row, cmdId, values) {
            if (row !== page.deviceRow)
                return;
            if (cmdId === 26) {
                page.img = values;
                page.imgReady = true;
            } else if (cmdId === 342 && values.type !== undefined) {
                var a = Object.assign({}, page.aiSens);
                a[values.type] = values;
                page.aiSens = a;
            } else if (cmdId === 54) {
                page.rec = values;
                page.recReady = true;
            } else if (cmdId === 46) {
                page.md = values;
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing
        spacing: Theme.spacing

        // ---- Category sidebar ----
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.radius

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacing
                spacing: 6

                Text { text: qsTr("Camera:"); color: Theme.textMuted; font.pixelSize: 11 }
                CameraComboBox {
                    id: deviceCombo
                    Layout.fillWidth: true
                    onCurrentIndexChanged: {
                        page.deviceRow = currentIndex;
                        page.refreshAdmin();
                        page.fetch();
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                Repeater {
                    model: page.categories
                    Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        height: 34
                        radius: 4
                        color: page.category === modelData.key ? Theme.accentDim
                             : catHover.hovered ? Theme.surfaceAlt : "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            text: modelData.label
                            color: page.category === modelData.key ? Theme.text : Theme.textMuted
                            font.pixelSize: 13
                        }
                        HoverHandler { id: catHover }
                        TapHandler { onTapped: { page.category = modelData.key; page.fetch(); } }
                    }
                }
                Item { Layout.fillHeight: true }
                Text {
                    visible: !page.isAdmin && page.deviceRow >= 0
                    Layout.fillWidth: true
                    text: qsTr("Read-only (not an admin account)")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }

        // ---- Panel ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.radius

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacing * 2
                spacing: Theme.spacing

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: {
                            for (var i = 0; i < page.categories.length; i++)
                                if (page.categories[i].key === page.category)
                                    return page.categories[i].label;
                            return "";
                        }
                        color: Theme.text
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Text { text: page.status; color: Theme.textMuted; font.pixelSize: 12 }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                Loader {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    sourceComponent: {
                        switch (page.category) {
                        case "image": return imagePanel;
                        case "encoding": return encodingPanel;
                        case "display": return displayPanel;
                        case "detection": return detectionPanel;
                        case "lights": return lightsPanel;
                        case "recording": return recordingPanel;
                        case "network": return networkPanel;
                        case "storage": return storagePanel;
                        case "users": return usersPanel;
                        case "time": return timePanel;
                        case "system": return systemPanel;
                        default: return genericPanel;
                        }
                    }
                }
            }
        }
    }

    // Add-user / change-password dialog for the Users panel.
    UserEditDialog { id: userDialog; deviceRow: page.deviceRow }
    DetectionZoneDialog { id: zoneDialog }
    PrivacyMaskDialog { id: maskDialog }
    ScheduleDialog { id: schedDialog }

    // Confirmation sheet for destructive actions (e.g. disk format).
    ConfirmDialog {
        id: confirmDialog
        onConfirmed: (payload) => {
            if (payload && payload.action === "format")
                Devices.applySetting(page.deviceRow, "Format", { "HddInfo": { "id": [payload.id] } });
            if (payload && payload.action === "firmware")
                page.startFirmwareUpdate();
        }
    }
    // ---- Firmware (CheckFirmware / UpgradeOnline / UpgradeStatus) ----
    // The camera downloads and installs Reolink's newer firmware itself; it
    // reboots at the end. Offered only when CheckFirmware reports one.
    property bool fwNew: false
    property bool fwChecking: false
    property bool fwUpgrading: false
    property int fwPercent: 0
    readonly property string fwText: {
        AppSettings.localeName; // re-render on a language switch
        if (fwUpgrading) return qsTr("Updating… %1%  — the camera restarts when done").arg(fwPercent);
        if (fwChecking) return qsTr("Checking…");
        if (fwChecked) return fwNew ? qsTr("A new firmware version is available.")
                                    : qsTr("The firmware is up to date.");
        return "";
    }
    property bool fwChecked: false
    function checkFirmware() {
        fwChecking = true;
        Devices.fetchSettings(page.deviceRow, ["CheckFirmware"], "fw");
    }
    function confirmFirmware() {
        confirmDialog.message = qsTr("Update the firmware of %1?").arg(Devices.nameAt(page.deviceRow));
        confirmDialog.detail = qsTr("The camera downloads the update from Reolink, installs it and restarts. It is offline for a few minutes; do not unplug it meanwhile.");
        confirmDialog.confirmLabel = qsTr("Update");
        confirmDialog.payload = { action: "firmware" };
        confirmDialog.open();
    }
    function startFirmwareUpdate() {
        fwUpgrading = true;
        fwPercent = 0;
        Devices.applySetting(page.deviceRow, "UpgradeOnline", {});
        fwPoll.restart();
    }
    Timer {
        id: fwPoll
        interval: 5000; repeat: true
        property int ticks: 0
        onTriggered: {
            if (++ticks > 60 || !page.fwUpgrading) { stop(); ticks = 0; page.fwUpgrading = false; return; }
            Devices.fetchSettings(page.deviceRow, ["UpgradeStatus"], "fw");
        }
    }
    Connections {
        target: Devices
        function onSettingsLoaded(row, values) {
            if (row !== page.deviceRow || values["_tag"] !== "fw") return;
            if (values["CheckFirmware"] !== undefined) {
                page.fwChecking = false;
                page.fwChecked = true;
                page.fwNew = values["CheckFirmware"].newFirmware === 1;
            }
            if (values["UpgradeStatus"] !== undefined) {
                var st = values["UpgradeStatus"].Status || {};
                page.fwPercent = st.Persent || page.fwPercent;
            }
            if (values["_error"] && page.fwChecking) {
                page.fwChecking = false;
                page.setStatus(function () { return values["_error"]; });
            }
        }
    }
    onDeviceRowChanged: { fwNew = false; fwChecked = false; fwChecking = false; fwUpgrading = false; }

    function confirmFormat(diskId, label) {
        confirmDialog.message = qsTr("Format %1?").arg(label);
        confirmDialog.detail = qsTr("This permanently erases ALL recordings on the disk and cannot be undone.");
        confirmDialog.confirmLabel = qsTr("Format disk");
        confirmDialog.payload = { action: "format", id: diskId };
        confirmDialog.open();
    }
    function openUserDialog(mode, name) {
        userDialog.mode = mode;
        userDialog.userName = name || "";
        userDialog.open();
    }

    // ---- Reusable form controls -------------------------------------------
    // A labeled slider that writes once, on release (so dragging doesn't spam the
    // NVR). `commit` receives the integer value.
    component SliderRow: RowLayout {
        property string label: ""
        property int from: 0
        property int to: 255
        property int value: 0
        property bool enabledCtl: true
        signal commit(int v)
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text { text: parent.label; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 170
               wrapMode: Text.WordWrap }
        Slider {
            id: sld
            Layout.fillWidth: true
            enabled: parent.enabledCtl
            from: parent.from; to: parent.to
            value: parent.value
            stepSize: 1
            onPressedChanged: if (!pressed) parent.commit(Math.round(value))
        }
        Text { text: Math.round(sld.value); color: Theme.text; font.pixelSize: 12; Layout.preferredWidth: 32 }
    }

    // A labeled dropdown of string options; writes the chosen option on change.
    component EnumRow: RowLayout {
        property string label: ""
        property var options: []
        property string value: ""
        property bool enabledCtl: true
        signal commit(string v)
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text { text: parent.label; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 170
               wrapMode: Text.WordWrap }
        ComboBox {
            id: cb
            Layout.preferredWidth: 200
            enabled: parent.enabledCtl
            model: parent.options
            currentIndex: Math.max(0, parent.options.indexOf(parent.value))
            onActivated: parent.commit(currentText)
        }
        Item { Layout.fillWidth: true }
    }

    component SwitchRow: RowLayout {
        property string label: ""
        property bool checked: false
        property bool enabledCtl: true
        signal commit(bool v)
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text { text: parent.label; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 170
               wrapMode: Text.WordWrap }
        Switch { checked: parent.checked; enabled: parent.enabledCtl; onToggled: parent.commit(checked) }
        Item { Layout.fillWidth: true }
    }

    // A dropdown over device values with readable, translated labels. `values`
    // usually comes from the command's range block, so only what the camera
    // accepts is offered.
    component ChoiceRow: RowLayout {
        property string label: ""
        property var values: []
        property string value: ""
        property bool enabledCtl: true
        signal commit(string v)
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text { text: parent.label; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 170
               wrapMode: Text.WordWrap }
        ThemedComboBox {
            Layout.preferredWidth: 240
            enabled: parent.enabledCtl
            model: parent.values.map(page.choiceLabel)
            currentIndex: Math.max(0, parent.values.indexOf(parent.value))
            onActivated: (i) => parent.commit(parent.values[i])
        }
        Item { Layout.fillWidth: true }
    }
    function choiceLabel(v) {
        switch (String(v)) {
        case "Auto": return qsTr("Auto");
        case "Manual": return qsTr("Manual");
        case "On": return qsTr("On");
        case "Off": return qsTr("Off");
        case "Other": return qsTr("Other");
        case "LowNoise": return qsTr("Low noise");
        case "Anti-Smearing": return qsTr("Anti-smearing (fast motion)");
        case "BackLightControl": return qsTr("Backlight compensation (BLC)");
        case "DynamicRangeControl": return qsTr("Dynamic range (DRC)");
        case "Color": return qsTr("Colour");
        case "Black&White": return qsTr("Black & white");
        // Baichuan day/night values (image panel)
        case "auto": return qsTr("Auto");
        case "color": return qsTr("Colour");
        case "blackAndWhite": return qsTr("Black & white");
        case "low": return qsTr("Low");
        case "medium": return qsTr("Medium");
        case "high": return qsTr("High");
        case "50HZ": return "50 Hz";
        case "60HZ": return "60 Hz";
        // Floodlight night modes (WhiteLed.mode)
        case "0": return qsTr("Off");
        case "1": return qsTr("On detection, at night");
        case "3": return qsTr("On a schedule");
        default: return String(v);
        }
    }

    // ---- Image panel (over native Baichuan — no HTTP 502) ----------------
    Component {
        id: imagePanel
        Item {
            property bool noData: page.imgReady && Object.keys(page.img).length === 0
            ColumnLayout {
                anchors.fill: parent
                visible: parent.noData
                spacing: Theme.spacing
                Text { text: qsTr("Image settings couldn't be read from the camera.")
                       color: Theme.text; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Item { Layout.fillHeight: true }
            }
            ScrollView {
                anchors.fill: parent
                visible: !parent.noData
                contentWidth: availableWidth
                clip: true
            ColumnLayout {
                width: parent.width
                spacing: Theme.spacing
                SliderRow {
                    label: qsTr("Brightness"); enabledCtl: page.isAdmin
                    value: parseInt(page.img.bright || "128")
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "bright": v })
                }
                SliderRow {
                    label: qsTr("Contrast"); enabledCtl: page.isAdmin
                    value: parseInt(page.img.contrast || "128")
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "contrast": v })
                }
                SliderRow {
                    label: qsTr("Saturation"); enabledCtl: page.isAdmin
                    value: parseInt(page.img.saturation || "128")
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "saturation": v })
                }
                SliderRow {
                    label: qsTr("Sharpness"); enabledCtl: page.isAdmin
                    value: parseInt(page.img.sharpen || "128")
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "sharpen": v })
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                SwitchRow {
                    label: qsTr("Mirror (horizontal)"); enabledCtl: page.isAdmin
                    checked: parseInt(page.img.mirror || "0") === 1
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "mirror": v ? 1 : 0 })
                }
                SwitchRow {
                    label: qsTr("Flip (vertical)"); enabledCtl: page.isAdmin
                    checked: parseInt(page.img.flip || "0") === 1
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "flip": v ? 1 : 0 })
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                ChoiceRow {
                    label: qsTr("Day / Night"); enabledCtl: page.isAdmin
                    values: ["auto", "color", "blackAndWhite"]
                    value: page.img["DayNight/mode"] || "auto"
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "DayNight/mode": v })
                }
                ChoiceRow {
                    label: qsTr("Day/Night threshold"); enabledCtl: page.isAdmin
                    values: ["low", "medium", "high"]
                    value: page.img["DayNight/Threshold"] || "medium"
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 26, 25, { "DayNight/Threshold": v })
                }
                Text {
                    text: qsTr("Over the native Baichuan protocol — no web-server timeouts.")
                    color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap
                }

                // ---- Advanced (HTTP GetIsp / GetIrLights / GetPowerLed) ----
                // Each control appears only when the camera reports the field,
                // and offers only the values its range block allows.
                Text {
                    visible: page.has("GetIsp")
                    text: qsTr("Advanced"); color: Theme.text; font.pixelSize: 14; font.bold: true
                    Layout.topMargin: Theme.spacing
                }
                Rectangle { visible: page.has("GetIsp"); Layout.fillWidth: true; height: 1; color: Theme.border }
                ChoiceRow {
                    readonly property var isp: page.val("GetIsp", "Isp") || ({})
                    visible: isp.exposure !== undefined
                    label: qsTr("Exposure"); enabledCtl: page.isAdmin
                    values: page.rng("GetIsp", "Isp", "exposure").length ? page.rng("GetIsp", "Isp", "exposure") : ["Auto"]
                    value: isp.exposure || ""
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": { "channel": page.ch(), "exposure": v } })
                }
                ChoiceRow {
                    readonly property var isp: page.val("GetIsp", "Isp") || ({})
                    visible: isp.antiFlicker !== undefined
                    label: qsTr("Anti-flicker"); enabledCtl: page.isAdmin
                    values: page.rng("GetIsp", "Isp", "antiFlicker").length ? page.rng("GetIsp", "Isp", "antiFlicker") : ["Off"]
                    value: isp.antiFlicker || ""
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": { "channel": page.ch(), "antiFlicker": v } })
                }
                ChoiceRow {
                    id: backlightRow
                    readonly property var isp: page.val("GetIsp", "Isp") || ({})
                    visible: isp.backLight !== undefined
                    label: qsTr("Backlight"); enabledCtl: page.isAdmin
                    values: page.rng("GetIsp", "Isp", "backLight").length ? page.rng("GetIsp", "Isp", "backLight") : ["Off"]
                    value: isp.backLight || ""
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": { "channel": page.ch(), "backLight": v } })
                }
                SliderRow {
                    // BLC and DRC each have their own strength.
                    readonly property string key: backlightRow.value === "BackLightControl" ? "blc" : "drc"
                    visible: backlightRow.visible && backlightRow.value !== "Off" && backlightRow.isp[key] !== undefined
                    label: qsTr("Backlight strength"); enabledCtl: page.isAdmin
                    from: page.rng("GetIsp", "Isp", key).min || 0
                    to: page.rng("GetIsp", "Isp", key).max || 255
                    value: backlightRow.isp[key] || 128
                    onCommit: (v) => {
                        var isp = { "channel": page.ch() };
                        isp[key] = v;
                        Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": isp });
                    }
                }
                ChoiceRow {
                    id: wbRow
                    readonly property var isp: page.val("GetIsp", "Isp") || ({})
                    visible: isp.whiteBalance !== undefined
                    label: qsTr("White balance"); enabledCtl: page.isAdmin
                    values: page.rng("GetIsp", "Isp", "whiteBalance").length ? page.rng("GetIsp", "Isp", "whiteBalance") : ["Auto"]
                    value: isp.whiteBalance || ""
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": { "channel": page.ch(), "whiteBalance": v } })
                }
                SliderRow {
                    visible: wbRow.visible && wbRow.value === "Manual" && wbRow.isp.redGain !== undefined
                    label: qsTr("Red gain"); enabledCtl: page.isAdmin
                    from: page.rng("GetIsp", "Isp", "redGain").min || 0
                    to: page.rng("GetIsp", "Isp", "redGain").max || 255
                    value: wbRow.isp.redGain || 128
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": { "channel": page.ch(), "redGain": v } })
                }
                SliderRow {
                    visible: wbRow.visible && wbRow.value === "Manual" && wbRow.isp.blueGain !== undefined
                    label: qsTr("Blue gain"); enabledCtl: page.isAdmin
                    from: page.rng("GetIsp", "Isp", "blueGain").min || 0
                    to: page.rng("GetIsp", "Isp", "blueGain").max || 255
                    value: wbRow.isp.blueGain || 128
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": { "channel": page.ch(), "blueGain": v } })
                }
                SwitchRow {
                    readonly property var isp: page.val("GetIsp", "Isp") || ({})
                    visible: isp.nr3d !== undefined
                    label: qsTr("3D noise reduction"); enabledCtl: page.isAdmin
                    checked: isp.nr3d === 1
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIsp", { "Isp": { "channel": page.ch(), "nr3d": v ? 1 : 0 } })
                }
                ChoiceRow {
                    visible: page.has("GetIrLights")
                    label: qsTr("Infrared lights"); enabledCtl: page.isAdmin
                    values: page.rng("GetIrLights", "IrLights", "state").length ? page.rng("GetIrLights", "IrLights", "state") : ["Auto", "Off"]
                    value: page.val("GetIrLights", "IrLights", "state") || ""
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetIrLights", { "IrLights": { "channel": page.ch(), "state": v } })
                }
                ChoiceRow {
                    visible: page.has("GetPowerLed")
                    label: qsTr("Status LED"); enabledCtl: page.isAdmin
                    values: page.rng("GetPowerLed", "PowerLed", "state").length ? page.rng("GetPowerLed", "PowerLed", "state") : ["On", "Off"]
                    value: page.val("GetPowerLed", "PowerLed", "state") || ""
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetPowerLed", { "PowerLed": { "channel": page.ch(), "state": v } })
                }
                Item { height: Theme.spacing }
            }
            }
        }
    }

    // ---- Detection / Alerts panel (motion + AI types + push) --------------
    Component {
        id: detectionPanel
        ColumnLayout {
            spacing: Theme.spacing
            Text { text: qsTr("Motion detection"); color: Theme.text; font.pixelSize: 13; font.bold: true }
            SliderRow {
                label: qsTr("Sensitivity"); from: 1; to: 50; enabledCtl: page.isAdmin
                value: parseInt(page.md.sensitivity || "25")
                onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 46, 47,
                    { "sensitivityInfoList//sensitivity": v })
            }
            // Detection-zone (grid mask) editor — disable detection over parts of
            // the frame, like the Windows app. Enabled once MdAlarm has loaded.
            Rectangle {
                Layout.fillWidth: true; implicitHeight: 36; radius: Theme.radius
                readonly property bool ready: page.isAdmin && page.md.valueTable !== undefined
                color: dzHover.hovered && ready ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                opacity: ready ? 1 : 0.5
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                    Text { text: qsTr("Detection zone"); color: Theme.text; font.pixelSize: 13; Layout.fillWidth: true }
                    Text { text: qsTr("Edit\u2026"); color: Theme.accent; font.pixelSize: 12 }
                }
                HoverHandler { id: dzHover }
                TapHandler { onTapped: if (parent.ready) zoneDialog.openFor(page.deviceRow, page.md) }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
            Text { text: qsTr("Smart (AI) detection"); color: Theme.text; font.pixelSize: 13; font.bold: true }
            SwitchRow {
                label: qsTr("People"); enabledCtl: page.isAdmin
                checked: page.val("GetAiCfg","AiDetectType","people") === 1
                onCommit: (v) => Devices.applySetting(page.deviceRow, "SetAiCfg",
                    { "channel": Devices.channelOf(page.deviceRow), "AiDetectType": { "people": v ? 1 : 0 } })
            }
            SliderRow {
                label: qsTr("  People sensitivity"); from: 0; to: 100; enabledCtl: page.isAdmin
                visible: page.aiSens["people"] !== undefined
                value: parseInt((page.aiSens["people"] && page.aiSens["people"].sensitivity) || "50")
                onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 342, 343, { "sensitivity": v }, page.aiBody("people"))
            }

            SwitchRow {
                label: qsTr("Vehicles"); enabledCtl: page.isAdmin
                checked: page.val("GetAiCfg","AiDetectType","vehicle") === 1
                onCommit: (v) => Devices.applySetting(page.deviceRow, "SetAiCfg",
                    { "channel": Devices.channelOf(page.deviceRow), "AiDetectType": { "vehicle": v ? 1 : 0 } })
            }
            SliderRow {
                label: qsTr("  Vehicle sensitivity"); from: 0; to: 100; enabledCtl: page.isAdmin
                visible: page.aiSens["vehicle"] !== undefined
                value: parseInt((page.aiSens["vehicle"] && page.aiSens["vehicle"].sensitivity) || "50")
                onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 342, 343, { "sensitivity": v }, page.aiBody("vehicle"))
            }

            SwitchRow {
                label: qsTr("Animals (pets)"); enabledCtl: page.isAdmin
                checked: page.val("GetAiCfg","AiDetectType","dog_cat") === 1
                onCommit: (v) => Devices.applySetting(page.deviceRow, "SetAiCfg",
                    { "channel": Devices.channelOf(page.deviceRow), "AiDetectType": { "dog_cat": v ? 1 : 0 } })
            }
            SliderRow {
                label: qsTr("  Animal sensitivity"); from: 0; to: 100; enabledCtl: page.isAdmin
                visible: page.aiSens["dog_cat"] !== undefined
                value: parseInt((page.aiSens["dog_cat"] && page.aiSens["dog_cat"].sensitivity) || "50")
                onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 342, 343, { "sensitivity": v }, page.aiBody("dog_cat"))
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
            Text { text: qsTr("Alerts"); color: Theme.text; font.pixelSize: 13; font.bold: true }
            SwitchRow {
                label: qsTr("Push notifications")
                enabledCtl: page.isAdmin && page.alerts.ok === true
                checked: page.alerts.push === 1
                onCommit: (v) => Devices.setAlertEnable(page.deviceRow, "push", v)
            }
            SwitchRow {
                label: qsTr("Email on alarm")
                enabledCtl: page.isAdmin && page.alerts.ok === true
                checked: page.alerts.email === 1
                onCommit: (v) => Devices.setAlertEnable(page.deviceRow, "email", v)
            }
            SwitchRow {
                label: qsTr("FTP upload on alarm")
                enabledCtl: page.isAdmin && page.alerts.ok === true
                checked: page.alerts.ftp === 1
                onCommit: (v) => Devices.setAlertEnable(page.deviceRow, "ftp", v)
            }
            Text {
                visible: page.alerts.ok === false
                text: qsTr("Alert settings couldn't be read from the device.")
                color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap
            }
            Item { Layout.fillHeight: true }
            Text {
                text: qsTr("Detection zones and per-type sensitivity/schedules are on the roadmap.")
                color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap
            }
        }
    }

    // ---- Encoding panel ----
    // ---- Lights & siren panel (HTTP GetWhiteLed / GetAudioAlarmV20 / GetAudioCfg) ----
    Component {
        id: lightsPanel
        ScrollView {
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: lights
                width: parent.width
                spacing: Theme.spacing
                readonly property var wl: page.val("GetWhiteLed", "WhiteLed") || ({})
                readonly property var sched: wl.LightingSchedule || ({})
                readonly property var aiTypes: wl.wlAiDetectType || ({})
                // SetWhiteLed wants channel/state/mode/bright together (reolink_aio).
                function writeLed(changes) {
                    var w = { "channel": page.ch(), "state": lights.wl.state || 0,
                              "mode": lights.wl.mode || 0, "bright": lights.wl.bright || 0 };
                    for (var k in changes) w[k] = changes[k];
                    Devices.applySetting(page.deviceRow, "SetWhiteLed", { "WhiteLed": w });
                }

                Text {
                    text: page.fetched && !page.has("GetWhiteLed") && !page.has("GetAudioAlarmV20") && !page.has("GetAudioCfg")
                          ? qsTr("This camera reports no floodlight, siren or speaker settings.") : ""
                    visible: text.length > 0
                    color: Theme.textMuted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap
                }

                // ---- Floodlight ----
                Text { visible: page.has("GetWhiteLed"); text: qsTr("Floodlight")
                       color: Theme.text; font.pixelSize: 14; font.bold: true }
                ChoiceRow {
                    visible: page.has("GetWhiteLed")
                    label: qsTr("Night mode"); enabledCtl: page.isAdmin
                    // reolink_aio SpotlightModeEnum: 0 off, 1 auto (on detection
                    // at night), 3 on a schedule.
                    values: ["0", "1", "3"]
                    value: String(lights.wl.mode !== undefined ? lights.wl.mode : 0)
                    onCommit: (v) => lights.writeLed({ "mode": parseInt(v) })
                }
                SliderRow {
                    visible: page.has("GetWhiteLed")
                    label: qsTr("Brightness"); enabledCtl: page.isAdmin
                    from: page.rng("GetWhiteLed", "WhiteLed", "bright").min || 0
                    to: page.rng("GetWhiteLed", "WhiteLed", "bright").max || 100
                    value: lights.wl.bright || 0
                    onCommit: (v) => lights.writeLed({ "bright": v })
                }
                RowLayout {
                    visible: page.has("GetWhiteLed") && lights.wl.mode === 3
                    spacing: Theme.spacing
                    Text { text: qsTr("On from"); color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 150 }
                    component HourSpin: ThemedSpinBox {
                        from: 0; to: 23; implicitWidth: 80; enabled: page.isAdmin
                        textFromValue: function (v) { return (v < 10 ? "0" : "") + v + ":00"; }
                        valueFromText: function (t) { return parseInt(t); }
                    }
                    HourSpin {
                        value: lights.sched.StartHour || 0
                        onValueModified: lights.writeLed({ "LightingSchedule": { "StartHour": value, "StartMin": 0,
                            "EndHour": lights.sched.EndHour || 0, "EndMin": 0 } })
                    }
                    Text { text: qsTr("to"); color: Theme.textMuted; font.pixelSize: 12 }
                    HourSpin {
                        value: lights.sched.EndHour || 0
                        onValueModified: lights.writeLed({ "LightingSchedule": { "StartHour": lights.sched.StartHour || 0,
                            "StartMin": 0, "EndHour": value, "EndMin": 0 } })
                    }
                    Item { Layout.fillWidth: true }
                }
                RowLayout {
                    visible: page.has("GetWhiteLed") && lights.wl.mode === 1 && lights.wl.wlAiDetectType !== undefined
                    spacing: Theme.spacing
                    Text { text: qsTr("Turn on for"); color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 150 }
                    Repeater {
                        model: [{ key: "people", label: qsTr("People") }, { key: "vehicle", label: qsTr("Vehicles") },
                                { key: "dog_cat", label: qsTr("Animals") }]
                        delegate: ThemedCheckBox {
                            required property var modelData
                            visible: lights.aiTypes[modelData.key] !== undefined
                            enabled: page.isAdmin
                            text: modelData.label
                            checked: lights.aiTypes[modelData.key] === 1
                            onToggled: {
                                var t = Object.assign({}, lights.aiTypes);
                                t[modelData.key] = checked ? 1 : 0;
                                lights.writeLed({ "wlAiDetectType": t });
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                // ---- Siren ----
                Text { visible: page.has("GetAudioAlarmV20"); text: qsTr("Siren")
                       color: Theme.text; font.pixelSize: 14; font.bold: true; Layout.topMargin: Theme.spacing }
                SwitchRow {
                    visible: page.has("GetAudioAlarmV20")
                    label: qsTr("Sound the siren on alarm"); enabledCtl: page.isAdmin
                    checked: page.val("GetAudioAlarmV20", "Audio", "enable") === 1
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetAudioAlarmV20",
                        { "Audio": { "enable": v ? 1 : 0, "schedule": { "channel": page.ch() } } })
                }
                Text {
                    visible: page.has("GetAudioAlarmV20")
                    text: qsTr("Test it from Live View with the siren button.")
                    color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap
                }

                // ---- Speaker ----
                Text { visible: page.has("GetAudioCfg"); text: qsTr("Speaker")
                       color: Theme.text; font.pixelSize: 14; font.bold: true; Layout.topMargin: Theme.spacing }
                SliderRow {
                    visible: page.has("GetAudioCfg")
                    label: qsTr("Volume"); enabledCtl: page.isAdmin
                    from: page.rng("GetAudioCfg", "AudioCfg", "volume").min || 0
                    to: page.rng("GetAudioCfg", "AudioCfg", "volume").max || 100
                    value: page.val("GetAudioCfg", "AudioCfg", "volume") || 0
                    onCommit: (v) => Devices.applySetting(page.deviceRow, "SetAudioCfg",
                        { "AudioCfg": { "channel": page.ch(), "volume": v } })
                }
                Item { height: Theme.spacing }
            }
        }
    }

    Component {
        id: encodingPanel
        ColumnLayout {
            spacing: Theme.spacing
            SettingRow {
                label: qsTr("Main stream (Clear)")
                Text {
                    text: (page.val("GetEnc","Enc","mainStream","size") || "—") + "  ·  "
                        + (page.val("GetEnc","Enc","mainStream","bitRate") || "—") + " kbps  ·  "
                        + (page.val("GetEnc","Enc","mainStream","frameRate") || "—") + " fps  ·  "
                        + String(page.val("GetEnc","Enc","mainStream","vType") || "—").toUpperCase()
                    color: Theme.textMuted; font.pixelSize: 12
                }
            }
            SettingRow {
                label: qsTr("Sub stream (Fluent)")
                Text {
                    text: (page.val("GetEnc","Enc","subStream","size") || "—") + "  ·  "
                        + (page.val("GetEnc","Enc","subStream","bitRate") || "—") + " kbps  ·  "
                        + (page.val("GetEnc","Enc","subStream","frameRate") || "—") + " fps"
                    color: Theme.textMuted; font.pixelSize: 12
                }
            }
            SettingRow {
                label: qsTr("Audio")
                Switch {
                    checked: page.val("GetEnc","Enc","audio") === 1
                    enabled: page.isAdmin
                    onToggled: Devices.applySetting(page.deviceRow, "SetEnc",
                        { "Enc": { "channel": Devices.channelOf(page.deviceRow), "audio": checked ? 1 : 0 } })
                }
            }
            Item { Layout.fillHeight: true }
            Text {
                text: qsTr("Resolution/bitrate/framerate options populate from the device's GetEnc ranges.")
                color: Theme.textMuted; font.pixelSize: 11
            }
        }
    }

    // ---- Display / Image panel (OSD) ----
    Component {
        id: displayPanel
        ColumnLayout {
            spacing: Theme.spacing
            SettingRow {
                label: qsTr("Show camera name (OSD)")
                Switch {
                    checked: page.val("GetOsd","Osd","osdChannel","enable") === 1
                    enabled: page.isAdmin
                    onToggled: Devices.applySetting(page.deviceRow, "SetOsd",
                        { "Osd": { "channel": Devices.channelOf(page.deviceRow), "osdChannel": { "enable": checked ? 1 : 0 } } })
                }
            }
            SettingRow {
                label: qsTr("Show date/time (OSD)")
                Switch {
                    checked: page.val("GetOsd","Osd","osdTime","enable") === 1
                    enabled: page.isAdmin
                    onToggled: Devices.applySetting(page.deviceRow, "SetOsd",
                        { "Osd": { "channel": Devices.channelOf(page.deviceRow), "osdTime": { "enable": checked ? 1 : 0 } } })
                }
            }
            SettingRow {
                label: qsTr("Camera name")
                TextField {
                    Layout.preferredWidth: 200
                    text: page.val("GetOsd","Osd","osdChannel","name") || ""
                    enabled: page.isAdmin
                    color: Theme.text
                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    onEditingFinished: Devices.applySetting(page.deviceRow, "SetOsd",
                        { "Osd": { "channel": Devices.channelOf(page.deviceRow), "osdChannel": { "name": text } } })
                }
            }
            // ---- Privacy masks (GetMask / SetMask) ----
            Rectangle { visible: page.has("GetMask"); Layout.fillWidth: true; height: 1; color: Theme.border }
            SettingRow {
                visible: page.has("GetMask")
                label: qsTr("Privacy masks")
                hint: {
                    var a = page.val("GetMask", "Mask", "area");
                    var n = a ? a.length : 0;
                    return n === 0 ? qsTr("No area drawn yet") : qsTr("%n area(s) covered", "", n);
                }
                Switch {
                    checked: page.val("GetMask", "Mask", "enable") === 1
                    enabled: page.isAdmin
                    onToggled: Devices.applySetting(page.deviceRow, "SetMask",
                        { "Mask": { "channel": page.ch(), "enable": checked ? 1 : 0 } })
                }
                Rectangle {
                    implicitWidth: maskTxt.implicitWidth + 20; implicitHeight: 28; radius: 4
                    color: maskHover.hovered && page.isAdmin ? Theme.accentDim : Theme.surfaceAlt
                    border.color: Theme.border
                    opacity: page.isAdmin ? 1 : 0.5
                    Text { id: maskTxt; anchors.centerIn: parent; text: qsTr("Edit areas…"); color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: maskHover }
                    TapHandler { enabled: page.isAdmin
                                 onTapped: maskDialog.openFor(page.deviceRow, page.val("GetMask", "Mask"), page.rng("GetMask", "Mask")) }
                }
            }
            Item { Layout.fillHeight: true }
            Text {
                text: qsTr("Brightness, contrast, day/night and IR settings load from GetImage/GetIsp.")
                color: Theme.textMuted; font.pixelSize: 11
            }
        }
    }

    // ---- System panel ----
    Component {
        id: systemPanel
        ColumnLayout {
            spacing: Theme.spacing
            SettingRow { label: qsTr("Name")
                Text { text: page.val("GetDevInfo","DevInfo","name") || "—"; color: Theme.textMuted; font.pixelSize: 12 } }
            SettingRow { label: qsTr("Model")
                Text { text: page.val("GetDevInfo","DevInfo","model") || "—"; color: Theme.textMuted; font.pixelSize: 12 } }
            SettingRow { label: qsTr("Firmware")
                Text { text: page.val("GetDevInfo","DevInfo","firmVer") || "—"; color: Theme.textMuted; font.pixelSize: 12 } }
            SettingRow { label: qsTr("Hardware")
                Text { text: page.val("GetDevInfo","DevInfo","hardVer") || "—"; color: Theme.textMuted; font.pixelSize: 12 } }
            SettingRow { label: qsTr("Channels")
                Text { text: String(page.val("GetDevInfo","DevInfo","channelNum") || "—"); color: Theme.textMuted; font.pixelSize: 12 } }

            // ---- Firmware ----
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
            Text { text: qsTr("Firmware update"); color: Theme.text; font.pixelSize: 14; font.bold: true }
            SettingRow {
                visible: page.has("GetAutoUpgrade")
                label: qsTr("Install updates automatically")
                Switch {
                    checked: page.val("GetAutoUpgrade", "AutoUpgrade", "enable") === 1
                    enabled: page.isAdmin
                    onToggled: Devices.applySetting(page.deviceRow, "SetAutoUpgrade",
                        { "AutoUpgrade": { "enable": checked ? 1 : 0 } })
                }
            }
            SettingRow {
                label: qsTr("Online check")
                hint: page.fwText
                Rectangle {
                    implicitWidth: fwTxt.implicitWidth + 20; implicitHeight: 28; radius: 4
                    color: fwHover.hovered ? Theme.accentDim : Theme.surfaceAlt
                    border.color: Theme.border
                    Text { id: fwTxt; anchors.centerIn: parent; text: qsTr("Check now"); color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: fwHover }
                    TapHandler { onTapped: page.checkFirmware() }
                }
                Rectangle {
                    visible: page.fwNew && !page.fwUpgrading
                    implicitWidth: fwInstallTxt.implicitWidth + 20; implicitHeight: 28; radius: 4
                    color: fwInstallHover.hovered && page.isAdmin ? Theme.danger : Theme.surfaceAlt
                    border.color: Theme.danger
                    opacity: page.isAdmin ? 1 : 0.5
                    Text { id: fwInstallTxt; anchors.centerIn: parent; text: qsTr("Install update…"); color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: fwInstallHover }
                    TapHandler { enabled: page.isAdmin; onTapped: page.confirmFirmware() }
                }
            }
            Item { Layout.fillHeight: true }
            Rectangle {
                implicitWidth: rebootText.implicitWidth + 24
                height: 32
                radius: Theme.radius
                color: page.isAdmin ? (rebootHover.hovered ? Theme.danger : Theme.surfaceAlt) : Theme.surface
                border.color: page.isAdmin ? Theme.danger : Theme.border
                opacity: page.isAdmin ? 1 : 0.5
                Text {
                    id: rebootText
                    anchors.centerIn: parent
                    text: qsTr("Reboot device")
                    color: page.isAdmin ? Theme.text : Theme.textMuted
                    font.pixelSize: 12
                }
                HoverHandler { id: rebootHover; enabled: page.isAdmin }
                TapHandler { enabled: page.isAdmin; onTapped: Devices.reboot(page.deviceRow) }
            }
        }
    }

    // A read-only label:value row.
    component InfoRow: RowLayout {
        property string label: ""
        property string value: ""
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text { text: parent.label; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 160 }
        Text { text: parent.value.length ? parent.value : "—"; color: Theme.text; font.pixelSize: 12
               Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
    }

    // ---- Recording panel (camera-level) ----
    Component {
        id: recordingPanel
        Item {
            property bool noData: page.recReady && Object.keys(page.rec).length === 0
            ColumnLayout {
                anchors.fill: parent
                visible: parent.noData
                spacing: Theme.spacing
                Text { text: qsTr("Recording settings couldn't be read from the device.")
                       color: Theme.text; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Item { Layout.fillHeight: true }
            }
            ColumnLayout {
                anchors.fill: parent
                visible: !parent.noData
                spacing: Theme.spacing
                SwitchRow {
                    label: qsTr("Overwrite when full"); enabledCtl: page.isAdmin
                    checked: parseInt(page.rec.cycle || "1") === 1
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 54, 55, { "cycle": v ? 1 : 0 })
                }
                // Weekly per-type recording schedule (7x24 grid, cmd 81/82).
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: 36; radius: Theme.radius
                    readonly property bool ready: page.isAdmin && Object.keys(page.recSched).length > 0
                    color: schedHover.hovered && ready ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    opacity: ready ? 1 : 0.5
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                        Text { text: qsTr("Recording schedule"); color: Theme.text; font.pixelSize: 13; Layout.fillWidth: true }
                        Text { text: qsTr("Edit\u2026"); color: Theme.accent; font.pixelSize: 12 }
                    }
                    HoverHandler { id: schedHover }
                    TapHandler { onTapped: if (parent.ready) schedDialog.openFor(page.deviceRow, page.recSched) }
                }
                SliderRow {
                    label: qsTr("Pre-record (s)"); from: 0; to: 15; enabledCtl: page.isAdmin
                    value: parseInt(page.rec.preRecordTime || "0")
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 54, 55, { "preRecordTime": v })
                }
                SliderRow {
                    label: qsTr("Post-record (s)"); from: 0; to: 300; enabledCtl: page.isAdmin
                    value: parseInt(page.rec.recordDelayTime || "0")
                    onCommit: (v) => Devices.writeBcConfig(page.deviceRow, 54, 55, { "recordDelayTime": v })
                }
                InfoRow { label: qsTr("File split")
                    value: (page.rec.packageTime !== undefined ? page.rec.packageTime + qsTr(" min") : "") }
                Item { Layout.fillHeight: true }
                Text { text: qsTr("Over native Baichuan. The weekly schedule grid is on the roadmap.")
                       color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            }
        }
    }

    // ---- Network panel (host-level, read-only) ----
    Component {
        id: networkPanel
        ColumnLayout {
            spacing: Theme.spacing
            InfoRow { label: qsTr("Connection"); value: page.val("GetLocalLink","LocalLink","activeLink") || "" }
            InfoRow { label: qsTr("Addressing")
                value: page.val("GetLocalLink","LocalLink","type") === "DHCP" ? qsTr("DHCP") : qsTr("Static") }
            InfoRow { label: qsTr("IP address"); value: page.val("GetLocalLink","LocalLink","static","ip") || "" }
            InfoRow { label: qsTr("Gateway"); value: page.val("GetLocalLink","LocalLink","static","gateway") || "" }
            InfoRow { label: qsTr("Subnet mask"); value: page.val("GetLocalLink","LocalLink","static","mask") || "" }
            InfoRow { label: qsTr("MAC address"); value: page.val("GetLocalLink","LocalLink","mac") || "" }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
            Text { text: qsTr("Ports"); color: Theme.text; font.pixelSize: 13; font.bold: true }
            InfoRow { label: qsTr("RTSP")
                value: (page.val("GetNetPort","NetPort","rtspPort") || "—")
                     + (page.val("GetNetPort","NetPort","rtspEnable") === 1 ? "" : qsTr("  (disabled)")) }
            InfoRow { label: qsTr("ONVIF")
                value: (page.val("GetNetPort","NetPort","onvifPort") || "—")
                     + (page.val("GetNetPort","NetPort","onvifEnable") === 1 ? "" : qsTr("  (disabled)")) }
            InfoRow { label: qsTr("HTTPS")
                value: (page.val("GetNetPort","NetPort","httpsPort") || "—")
                     + (page.val("GetNetPort","NetPort","httpsEnable") === 1 ? "" : qsTr("  (disabled)")) }
            Item { Layout.fillHeight: true }
            Text { text: qsTr("Port and IP changes are read-only here — editing them from the app could lock out access.")
                   color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
    }

    // ---- Storage panel (host-level, read-only) ----
    Component {
        id: storagePanel
        ColumnLayout {
            spacing: Theme.spacing
            Repeater {
                model: page.val("GetHddInfo","HddInfo") || []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    InfoRow {
                        Layout.fillWidth: true
                        label: qsTr("Disk %1").arg((modelData.number !== undefined ? modelData.number : 0) + 1)
                        value: (modelData.storageType || qsTr("Disk"))
                             + "  ·  " + Math.round((modelData.capacity || 0) / 1024) + qsTr(" GB")
                             + "  ·  " + Math.round((modelData.size || 0) / 1024) + qsTr(" GB free")
                             + (modelData.mount ? "" : qsTr("  ·  not mounted"))
                             + (modelData.format ? "" : qsTr("  ·  unformatted"))
                    }
                    Rectangle {
                        visible: page.isAdmin
                        implicitWidth: fmtTxt.implicitWidth + 16; implicitHeight: 24; radius: 4
                        color: fmtHover.hovered ? Theme.danger : Theme.surfaceAlt
                        border.color: Theme.danger
                        Text { id: fmtTxt; anchors.centerIn: parent; text: qsTr("Format"); color: Theme.text; font.pixelSize: 11 }
                        HoverHandler { id: fmtHover }
                        TapHandler { onTapped: page.confirmFormat(modelData.number || 0,
                            qsTr("Disk %1").arg((modelData.number !== undefined ? modelData.number : 0) + 1)) }
                    }
                }
            }
            Text { visible: (page.val("GetHddInfo","HddInfo") || []).length === 0
                   text: page.status === "" ? qsTr("No storage reported.") : qsTr("Loading…")
                   color: Theme.textMuted; font.pixelSize: 12 }
            Item { Layout.fillHeight: true }
            Text { visible: !page.isAdmin
                   text: qsTr("Formatting a disk requires an administrator account.")
                   color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
    }

    // ---- Users panel (host-level) ----
    Component {
        id: usersPanel
        ColumnLayout {
            spacing: Theme.spacing

            component MiniBtn: Rectangle {
                property string label: ""
                property bool danger: false
                signal clicked()
                implicitWidth: mbTxt.implicitWidth + 16; implicitHeight: 24; radius: 4
                color: mbHover.hovered ? (danger ? Theme.danger : Theme.accentDim) : Theme.surfaceAlt
                border.color: danger ? Theme.danger : Theme.border
                Text { id: mbTxt; anchors.centerIn: parent; text: parent.label; color: Theme.text; font.pixelSize: 11 }
                HoverHandler { id: mbHover }
                TapHandler { onTapped: parent.clicked() }
            }

            Repeater {
                model: page.val("GetUser","User") || []
                delegate: RowLayout {
                    required property var modelData
                    property bool confirming: false
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    Text { text: "👤"; font.pixelSize: 12 }
                    Text { text: modelData.userName || "—"; color: Theme.text; font.pixelSize: 13; Layout.fillWidth: true }
                    Rectangle {
                        radius: 3; color: Theme.surfaceAlt; border.color: Theme.border
                        implicitWidth: lvl.implicitWidth + 12; implicitHeight: lvl.implicitHeight + 4
                        Text { id: lvl; anchors.centerIn: parent
                               text: modelData.level || ""; color: Theme.textMuted; font.pixelSize: 11 }
                    }
                    MiniBtn {
                        visible: page.isAdmin
                        label: qsTr("Password")
                        onClicked: page.openUserDialog("password", modelData.userName)
                    }
                    MiniBtn {
                        visible: page.isAdmin
                        label: parent.confirming ? qsTr("Confirm remove") : qsTr("Remove")
                        danger: parent.confirming
                        onClicked: {
                            if (parent.confirming) {
                                Devices.applySetting(page.deviceRow, "DelUser",
                                    { "User": { "userName": modelData.userName } });
                                parent.confirming = false;
                            } else {
                                parent.confirming = true;
                            }
                        }
                    }
                }
            }
            Text { visible: (page.val("GetUser","User") || []).length === 0
                   text: page.status === "" ? qsTr("No users reported.") : qsTr("Loading…")
                   color: Theme.textMuted; font.pixelSize: 12 }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; visible: page.isAdmin }
            MiniBtn {
                visible: page.isAdmin
                label: qsTr("+ Add user")
                onClicked: page.openUserDialog("add", "")
            }
            Item { Layout.fillHeight: true }
            Text { visible: !page.isAdmin
                   text: qsTr("Managing users requires an administrator account.")
                   color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
    }

    // ---- Time panel (host-level) ----
    Component {
        id: timePanel
        ColumnLayout {
            spacing: Theme.spacing
            InfoRow { label: qsTr("Device time")
                value: {
                    var t = page.val("GetTime","Time");
                    if (!t) return "";
                    function pad(n) { return String(n).padStart(2, "0"); }
                    return t.year + "-" + pad(t.mon) + "-" + pad(t.day) + "  "
                         + pad(t.hour) + ":" + pad(t.min) + ":" + pad(t.sec);
                } }
            InfoRow { label: qsTr("Time zone")
                value: {
                    var tz = page.val("GetTime","Time","timeZone");
                    if (tz === undefined) return "";
                    var h = -tz / 3600; // Reolink stores seconds west of UTC
                    return "UTC" + (h >= 0 ? "+" : "") + h;
                } }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
            Text { text: qsTr("Network time (NTP)"); color: Theme.text; font.pixelSize: 13; font.bold: true }
            SwitchRow {
                label: qsTr("Sync from NTP"); enabledCtl: page.isAdmin
                checked: page.val("GetNtp","Ntp","enable") === 1
                onCommit: (v) => Devices.applySetting(page.deviceRow, "SetNtp",
                    { "Ntp": { "enable": v ? 1 : 0,
                               "server": page.val("GetNtp","Ntp","server") || "pool.ntp.org",
                               "port": page.val("GetNtp","Ntp","port") || 123,
                               "interval": page.val("GetNtp","Ntp","interval") || 1440 } })
            }
            RowLayout {
                Layout.fillWidth: true; spacing: Theme.spacing
                Text { text: qsTr("NTP server"); color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 160 }
                TextField {
                    id: ntpServer
                    Layout.fillWidth: true
                    enabled: page.isAdmin
                    text: page.val("GetNtp","Ntp","server") || ""
                    color: Theme.text
                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    onEditingFinished: if (text.length > 0) Devices.applySetting(page.deviceRow, "SetNtp",
                        { "Ntp": { "enable": page.val("GetNtp","Ntp","enable") || 1, "server": text,
                                   "port": page.val("GetNtp","Ntp","port") || 123,
                                   "interval": page.val("GetNtp","Ntp","interval") || 1440 } })
                }
            }
            Item { Layout.fillHeight: true }
            Text { text: qsTr("Time zone / manual-clock editing is on the roadmap.")
                   color: Theme.textMuted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
    }

    // ---- Generic panel (fetched raw values) ----
    Component {
        id: genericPanel
        ColumnLayout {
            spacing: Theme.spacing
            Text {
                text: page.deviceRow < 0 ? qsTr("Select a camera to load settings.")
                    : Object.keys(page.settings).length === 0
                        ? qsTr("Loading settings from the device…")
                        : qsTr("This category's controls populate from the device response.")
                color: Theme.textMuted
                font.pixelSize: 13
            }
            // Raw fetched values (useful until each category has a bespoke form).
            Text {
                Layout.fillWidth: true
                visible: Object.keys(page.settings).length > 0
                text: JSON.stringify(page.settings, null, 2)
                color: Theme.textMuted
                font.pixelSize: 10
                font.family: "monospace"
                wrapMode: Text.WrapAnywhere
            }
            Item { Layout.fillHeight: true }
        }
    }
}
