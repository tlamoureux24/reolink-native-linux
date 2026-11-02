import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Advanced PTZ panel, opened from the PtzPad: movement speed, zoom/focus
// positions and autofocus, presets, patrol, guard point and AI auto-tracking.
// Every section is gated on the channel's abilities (cameraInfo cap* flags),
// and slider bounds come from the device's own "range" block — they differ per
// model (zoom 0..34 vs 0..41, focus 0..230 vs 0..1104 on two RLC-823A).
Popup {
    id: root
    property int deviceRow: -1
    property int speed: 32
    signal speedPicked(int value)

    readonly property var info: deviceRow >= 0 ? Devices.cameraInfo(deviceRow) : ({})
    readonly property int channel: deviceRow >= 0 ? Devices.channelOf(deviceRow) : 0
    property var st: ({})
    property string status: ""
    // A message kept on screen must follow a live language switch: remember
    // how to build it and rebuild it when the language changes.
    property var _statusFn: null
    function setStatus(fn) {
        _statusFn = fn;
        root.status = fn ? fn() : "";
    }
    Connections {
        target: AppSettings
        function onLanguageChanged() { if (root._statusFn) root.status = root._statusFn(); }
    }
    property bool loading: false

    width: 360
    // Sized against the window, not the small pad it opens from: with margins
    // set, Qt keeps the popup inside the window and shrinks it if needed.
    height: Math.min(720, implicitHeight)
    padding: 0
    margins: 8
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.border
        radius: Theme.radius
    }

    function cap(name) { return !!(root.info && root.info[name]); }
    // values[cmd][key] with a fallback (fetchSettings maps cmd -> device "value").
    function v(cmd, key, dflt) {
        var o = root.st[cmd];
        if (o === undefined || o === null) return dflt;
        if (key === undefined) return o;
        return o[key] !== undefined ? o[key] : dflt;
    }

    function refresh() {
        if (deviceRow < 0) return;
        var cmds = [];
        if (cap("capZoomFocusPos")) cmds.push("GetZoomFocus");
        if (cap("capAutoFocus")) cmds.push("GetAutoFocus");
        if (cap("capPtzPreset")) cmds.push("GetPtzPreset");
        if (cap("capPtzPatrol")) cmds.push("GetPtzPatrol");
        if (cap("capPtzGuard")) cmds.push("GetPtzGuard");
        if (cap("capAutoTrack")) cmds.push("GetAiCfg");
        if (cmds.length === 0) return;
        root.loading = true;
        Devices.fetchSettings(deviceRow, cmds, "ptz");
    }
    function apply(cmd, param) {
        root.setStatus(function () { return qsTr("Saving…"); });
        Devices.applySetting(deviceRow, cmd, param);
    }

    onOpened: refresh()

    // The camera needs a moment before a GET reflects a SET.
    Timer { id: refetch; interval: 1500; onTriggered: root.refresh() }

    readonly property var ownCommands: ["StartZoomFocus", "SetAutoFocus", "SetPtzPreset",
                                        "SetPtzPatrol", "SetPtzGuard", "SetAiCfg"]
    Connections {
        target: Devices
        function onSettingsLoaded(row, values) {
            if (row !== root.deviceRow || values["_tag"] !== "ptz") return;
            root.loading = false;
            root.st = values;
            if (values["_error"]) root.setStatus(function () { return values["_error"]; });
        }
        function onSettingApplied(row, command, ok, error) {
            if (row !== root.deviceRow || root.ownCommands.indexOf(command) < 0) return;
            root.setStatus(function () { return ok ? qsTr("Saved") : command + ": " + error; });
            if (ok) refetch.restart();
        }
        function onPtzPatrolDone(row, ok, error) {
            if (row !== root.deviceRow) return;
            root.setStatus(function () { return ok ? "" : qsTr("Patrol: %1").arg(error); });
            refetch.restart();
        }
    }

    // ---- Small themed building blocks ---------------------------------------
    component Section: Text {
        color: Theme.text; font.pixelSize: 13; font.bold: true
        Layout.topMargin: 6
    }
    component Btn: Rectangle {
        property string label: ""
        property bool danger: false
        property bool primary: false
        property bool enabledBtn: true
        signal clicked()
        implicitWidth: btnTxt.implicitWidth + 16; implicitHeight: 26; radius: 4
        opacity: enabledBtn ? 1 : 0.4
        color: !enabledBtn ? Theme.surfaceAlt
             : btnHover.hovered ? (danger ? Theme.danger : Theme.accentDim)
             : primary ? Theme.accentDim : Theme.surfaceAlt
        border.color: danger ? Theme.danger : Theme.border
        Text { id: btnTxt; anchors.centerIn: parent; text: parent.label; color: Theme.text; font.pixelSize: 11 }
        HoverHandler { id: btnHover; enabled: parent.enabledBtn }
        TapHandler { enabled: parent.enabledBtn; onTapped: parent.clicked() }
    }
    // Basic-style CheckBox draws its label in the light palette's dark text.
    component LabeledSlider: RowLayout {
        property string label: ""
        property int from: 0
        property int to: 100
        property int value: 0
        signal commit(int v)
        Layout.fillWidth: true
        spacing: 6
        Text { text: parent.label; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 110 }
        Slider {
            id: s
            Layout.fillWidth: true
            from: parent.from; to: parent.to; stepSize: 1
            value: parent.value
            onPressedChanged: if (!pressed) parent.commit(Math.round(value))
        }
        Text { text: Math.round(s.value); color: Theme.text; font.pixelSize: 12; Layout.preferredWidth: 36 }
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Header
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacing
            Text {
                text: qsTr("PTZ — %1").arg(root.info.name || "")
                color: Theme.text; font.pixelSize: 14; font.bold: true
                elide: Text.ElideRight; Layout.fillWidth: true
            }
            BusyIndicator { running: root.loading; visible: running; implicitWidth: 18; implicitHeight: 18 }
            Btn { label: "✕"; onClicked: root.close() }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        ScrollView {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            // Always show the bar: the panel is taller than most panes, and a
            // hidden overlay bar left the lower sections undiscovered.
            ScrollBar.vertical.policy: ScrollBar.AlwaysOn

            ColumnLayout {
                width: parent.width
                spacing: 6

                Item { height: 2 }

                // ---- Speed ----
                ColumnLayout {
                    visible: root.cap("capPtzSpeed")
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing; Layout.rightMargin: Theme.spacing
                    Section { text: qsTr("Movement speed") }
                    LabeledSlider {
                        label: qsTr("Speed"); from: 1; to: 64; value: root.speed
                        onCommit: (val) => root.speedPicked(val)
                    }
                }

                // ---- Zoom / focus ----
                ColumnLayout {
                    visible: root.cap("capZoomFocusPos") && root.v("GetZoomFocusRange") !== undefined
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing; Layout.rightMargin: Theme.spacing
                    readonly property var zf: root.v("GetZoomFocus", "ZoomFocus", {})
                    readonly property var zr: root.v("GetZoomFocusRange", "ZoomFocus", {})
                    Section { text: qsTr("Zoom and focus") }
                    LabeledSlider {
                        label: qsTr("Zoom")
                        from: parent.zr.zoom && parent.zr.zoom.pos ? parent.zr.zoom.pos.min : 0
                        to: parent.zr.zoom && parent.zr.zoom.pos ? parent.zr.zoom.pos.max : 33
                        value: parent.zf.zoom ? parent.zf.zoom.pos : 0
                        onCommit: (val) => root.apply("StartZoomFocus",
                            { ZoomFocus: { channel: root.channel, op: "ZoomPos", pos: val } })
                    }
                    LabeledSlider {
                        label: qsTr("Focus")
                        from: parent.zr.focus && parent.zr.focus.pos ? parent.zr.focus.pos.min : 0
                        to: parent.zr.focus && parent.zr.focus.pos ? parent.zr.focus.pos.max : 223
                        value: parent.zf.focus ? parent.zf.focus.pos : 0
                        onCommit: (val) => root.apply("StartZoomFocus",
                            { ZoomFocus: { channel: root.channel, op: "FocusPos", pos: val } })
                    }
                }

                // ---- Autofocus (also on zoom-only cameras such as the RLC-811A) ----
                RowLayout {
                    visible: root.cap("capAutoFocus") && root.v("GetAutoFocus") !== undefined
                    Layout.leftMargin: Theme.spacing; Layout.rightMargin: Theme.spacing
                    Text { text: qsTr("Autofocus"); color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 110 }
                    Switch {
                        checked: root.v("GetAutoFocus", "AutoFocus", {}).disable === 0
                        onToggled: root.apply("SetAutoFocus",
                            { AutoFocus: { channel: root.channel, disable: checked ? 0 : 1 } })
                    }
                }

                // ---- Presets ----
                ColumnLayout {
                    id: presets
                    visible: root.cap("capPtzPreset")
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing; Layout.rightMargin: Theme.spacing
                    readonly property var all: root.v("GetPtzPreset", "PtzPreset", [])
                    readonly property var used: all.filter(function (p) { return p.enable === 1; })
                    // Ids below the device's declared minimum (1 on the RLC-823A,
                    // although GetPtzPreset lists slot 0) are refused with
                    // "param error" when enabling a new preset.
                    readonly property int minId: {
                        var r = root.v("GetPtzPresetRange", "PtzPreset", {});
                        return r.id && r.id.min !== undefined ? r.id.min : 1;
                    }
                    readonly property var free: all.filter(function (p) {
                        return p.enable !== 1 && p.id >= minId;
                    })
                    Section { text: qsTr("Presets") }
                    Text {
                        visible: presets.used.length === 0
                        text: qsTr("No preset yet. Aim the camera, then name and save its position below.")
                        color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Repeater {
                        model: presets.used
                        delegate: RowLayout {
                            required property var modelData
                            property bool confirming: false
                            Layout.fillWidth: true
                            spacing: 4
                            Text {
                                text: modelData.name; color: Theme.text; font.pixelSize: 12
                                elide: Text.ElideRight; Layout.fillWidth: true
                            }
                            Btn { label: qsTr("Go"); primary: true
                                  onClicked: Devices.ptzPreset(root.deviceRow, modelData.id) }
                            Btn { label: qsTr("Save here")
                                  visible: !parent.confirming
                                  onClicked: root.apply("SetPtzPreset", { PtzPreset: {
                                      channel: root.channel, enable: 1, id: modelData.id, name: modelData.name } }) }
                            Btn { label: parent.confirming ? qsTr("Delete?") : "🗑"; danger: parent.confirming
                                  onClicked: {
                                      if (!parent.confirming) { parent.confirming = true; return; }
                                      parent.confirming = false;
                                      root.apply("SetPtzPreset", { PtzPreset: {
                                          channel: root.channel, enable: 0, id: modelData.id, name: modelData.name } });
                                  } }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        TextField {
                            id: presetName
                            Layout.fillWidth: true
                            placeholderText: qsTr("New preset name")
                            maximumLength: 31
                            color: Theme.text
                            placeholderTextColor: Theme.textMuted
                            background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                        }
                        Btn {
                            label: qsTr("Save position")
                            enabledBtn: presetName.text.trim().length > 0 && presets.free.length > 0
                            onClicked: {
                                root.apply("SetPtzPreset", { PtzPreset: {
                                    channel: root.channel, enable: 1, id: presets.free[0].id,
                                    name: presetName.text.trim() } });
                                presetName.text = "";
                            }
                        }
                    }
                }

                // ---- Patrol ----
                ColumnLayout {
                    id: patrol
                    visible: root.cap("capPtzPatrol")
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing; Layout.rightMargin: Theme.spacing
                    // Firmware exposes several cruise slots but its range allows one.
                    readonly property var p: (root.v("GetPtzPatrol", "PtzPatrol", []) || [])[0] || null
                    readonly property var stops: p && p.preset ? p.preset : []
                    readonly property bool running: !!(p && p.running)
                    property bool editing: false
                    property var draft: ({})   // preset id -> dwell seconds
                    Section { text: qsTr("Patrol") }
                    Text {
                        visible: !patrol.editing
                        text: patrol.stops.length === 0
                              ? qsTr("No patrol route. Add at least two presets to it.")
                              : qsTr("%n stop(s)", "", patrol.stops.length)
                                + (patrol.running ? " — " + qsTr("running") : "")
                        color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        visible: !patrol.editing
                        spacing: 4
                        Btn { label: patrol.running ? qsTr("Stop") : qsTr("Start"); primary: !patrol.running
                              enabledBtn: patrol.p !== null && patrol.stops.length > 0
                              onClicked: Devices.ptzPatrol(root.deviceRow, patrol.p.id, !patrol.running) }
                        Btn { label: qsTr("Edit route")
                              enabledBtn: patrol.p !== null && presets.used.length > 0
                              onClicked: {
                                  var d = {};
                                  for (var i = 0; i < patrol.stops.length; i++)
                                      d[patrol.stops[i].id] = patrol.stops[i].dwellTime;
                                  patrol.draft = d;
                                  patrol.editing = true;
                              } }
                    }
                    Repeater {
                        model: patrol.editing ? presets.used : []
                        delegate: RowLayout {
                            required property var modelData
                            readonly property bool inRoute: patrol.draft[modelData.id] !== undefined
                            Layout.fillWidth: true
                            spacing: 4
                            ThemedCheckBox {
                                checked: parent.inRoute
                                onToggled: {
                                    var d = Object.assign({}, patrol.draft);
                                    if (checked) d[modelData.id] = 10; else delete d[modelData.id];
                                    patrol.draft = d;
                                }
                            }
                            Text { text: modelData.name; color: Theme.text; font.pixelSize: 12
                                   elide: Text.ElideRight; Layout.fillWidth: true }
                            ThemedSpinBox {
                                enabled: parent.inRoute
                                from: 1; to: 300
                                value: parent.inRoute ? patrol.draft[modelData.id] : 10
                                implicitWidth: 96
                                onValueModified: {
                                    var d = Object.assign({}, patrol.draft);
                                    d[modelData.id] = value;
                                    patrol.draft = d;
                                }
                            }
                            Text { text: qsTr("s"); color: Theme.textMuted; font.pixelSize: 11 }
                        }
                    }
                    RowLayout {
                        visible: patrol.editing
                        spacing: 4
                        Btn { label: qsTr("Save route"); primary: true
                              onClicked: {
                                  var stops = [];
                                  for (var i = 0; i < presets.used.length; i++) {
                                      var id = presets.used[i].id;
                                      if (patrol.draft[id] !== undefined)
                                          stops.push({ id: id, dwellTime: patrol.draft[id], speed: root.speed });
                                  }
                                  root.apply("SetPtzPatrol", { PtzPatrol: {
                                      channel: root.channel, enable: stops.length > 0 ? 1 : 0,
                                      id: patrol.p.id, name: patrol.p.name, preset: stops } });
                                  patrol.editing = false;
                              } }
                        Btn { label: qsTr("Cancel"); onClicked: patrol.editing = false }
                    }
                }

                // ---- Guard point ----
                ColumnLayout {
                    visible: root.cap("capPtzGuard") && root.v("GetPtzGuard") !== undefined
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing; Layout.rightMargin: Theme.spacing
                    readonly property var g: root.v("GetPtzGuard", "PtzGuard", {})
                    Section { text: qsTr("Guard point") }
                    Text {
                        text: qsTr("The camera returns here after it has been idle.")
                        color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        spacing: 6
                        Text { text: qsTr("Enabled"); color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 110 }
                        Switch {
                            checked: parent.parent.g.benable === 1
                            onToggled: root.apply("SetPtzGuard", { PtzGuard: {
                                channel: root.channel, cmdStr: "setPos", benable: checked ? 1 : 0 } })
                        }
                        Text { text: qsTr("after"); color: Theme.textMuted; font.pixelSize: 12 }
                        ThemedSpinBox {
                            id: guardSpin
                            from: 10; to: 300; implicitWidth: 100
                            value: parent.parent.g.timeout || 60
                            onValueModified: guardTimeout.restart()
                        }
                        // Debounced: one write after the user stops stepping.
                        Timer {
                            id: guardTimeout; interval: 800
                            onTriggered: root.apply("SetPtzGuard", { PtzGuard: {
                                channel: root.channel, cmdStr: "setPos", timeout: guardSpin.value } })
                        }
                        Text { text: qsTr("s"); color: Theme.textMuted; font.pixelSize: 11 }
                    }
                    RowLayout {
                        spacing: 4
                        Btn { label: qsTr("Go to guard point"); primary: true
                              enabledBtn: parent.parent.g.bexistPos === 1
                              onClicked: root.apply("SetPtzGuard", { PtzGuard: {
                                  channel: root.channel, cmdStr: "toPos" } }) }
                        Btn { label: qsTr("Set current position")
                              onClicked: root.apply("SetPtzGuard", { PtzGuard: {
                                  channel: root.channel, cmdStr: "setPos", bSaveCurrentPos: 1 } }) }
                    }
                }

                // ---- Auto-tracking ----
                ColumnLayout {
                    id: track
                    visible: root.cap("capAutoTrack") && root.v("GetAiCfg") !== undefined
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing; Layout.rightMargin: Theme.spacing
                    readonly property var ai: root.v("GetAiCfg") || {}
                    readonly property var aiRange: root.v("GetAiCfgRange") || {}
                    // Newer firmware switches tracking with bSmartTrack; aiTrack then
                    // holds the method (reolink_aio set_auto_tracking).
                    readonly property bool smart: ai.bSmartTrack !== undefined
                    readonly property bool on: smart ? ai.bSmartTrack === 1 : ai.aiTrack === 1
                    readonly property var types: ai.trackType || {}
                    readonly property var detect: ai.AiDetectType || {}
                    readonly property var backTimes: aiRange.aiStopBackTime || [10, 20, 30, 60]
                    function setTrack(changes) {
                        root.apply("SetAiCfg", Object.assign({ channel: root.channel }, changes));
                    }
                    Section { text: qsTr("Auto-tracking") }
                    RowLayout {
                        Text { text: qsTr("Follow"); color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 110 }
                        Switch {
                            checked: track.on
                            onToggled: track.setTrack(track.smart ? { bSmartTrack: checked ? 1 : 0 }
                                                                  : { aiTrack: checked ? 1 : 0 })
                        }
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: [{ key: "people", label: qsTr("People") },
                                    { key: "vehicle", label: qsTr("Vehicles") },
                                    { key: "dog_cat", label: qsTr("Animals") }]
                            delegate: ThemedCheckBox {
                                required property var modelData
                                visible: track.detect[modelData.key] === 1 && track.types[modelData.key] !== undefined
                                text: modelData.label
                                checked: track.types[modelData.key] === 1
                                onToggled: {
                                    var t = Object.assign({}, track.types);
                                    t[modelData.key] = checked ? 1 : 0;
                                    track.setTrack({ trackType: t });
                                }
                            }
                        }
                    }
                    RowLayout {
                        visible: track.ai.aiStopBackTime !== undefined
                        Text { text: qsTr("Return when the target stops for"); color: Theme.textMuted
                               font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        ThemedComboBox {
                            implicitWidth: 80
                            model: track.backTimes.map(function (s) { return s + " s"; })
                            currentIndex: Math.max(0, track.backTimes.indexOf(track.ai.aiStopBackTime))
                            onActivated: (i) => track.setTrack({ aiStopBackTime: track.backTimes[i] })
                        }
                    }
                    RowLayout {
                        visible: track.ai.aiDisappearBackTime !== undefined
                        readonly property var times: track.aiRange.aiDisappearBackTime || [10, 20, 30, 60]
                        Text { text: qsTr("Return when the target is lost for"); color: Theme.textMuted
                               font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        ThemedComboBox {
                            implicitWidth: 80
                            model: parent.times.map(function (s) { return s + " s"; })
                            currentIndex: Math.max(0, parent.times.indexOf(track.ai.aiDisappearBackTime))
                            onActivated: (i) => track.setTrack({ aiDisappearBackTime: parent.times[i] })
                        }
                    }
                }

                Item { height: 6 }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; visible: statusTxt.text.length > 0 }
        Text {
            id: statusTxt
            visible: text.length > 0
            text: root.status
            color: Theme.textMuted; font.pixelSize: 11
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.margins: Theme.spacing
        }
    }
}
