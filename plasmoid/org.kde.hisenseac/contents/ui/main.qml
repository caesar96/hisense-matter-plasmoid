import QtQuick
import QtQuick.Layouts

import org.kde.plasma.plasmoid
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.workspace.dbus as DBus

import org.kde.kirigami as Kirigami

PlasmoidItem {
    id: root

    // ---- state -----------------------------------------------------------
    property bool connected: false
    property bool commissioned: false
    property bool power: false
    property string mode: "off"
    property real setpoint: 25
    property real localTemp: 0
    property real minSetpoint: 16
    property real maxSetpoint: 32
    property string lastError: ""
    property bool provisioning: false
    property bool unpairing: false
    property string provisionError: ""
    property string provError: ""
    property bool confirmForget: false
    // Optimistic feedback: show "Forgetting…" the moment the button is pressed
    // instead of waiting for the daemon to report the in-progress flag.
    property bool forgetting: false
    property bool _sawUnpairing: false
    // Optimistic feedback: show "Pairing…" the moment the button is pressed,
    // and keep polling briskly until the daemon reports the outcome.
    property bool pairingRequested: false

    // ---- ConnectLife cloud extras (only meaningful for the cloud backend) --
    property string deviceId: ""
    property string fanSpeed: "auto"
    property bool superFast: false
    property bool sleep: false
    property bool fanMute: false
    property bool swing: false
    property bool dimmer: false
    property real humidity: 0
    // While a cloud write is in flight, keep showing the optimistic value and
    // do not let a slightly stale poll snapshot overwrite it.
    property bool extrasLock: false

    // Optimistic UI: a command updates the display immediately and is rolled
    // back if the device rejects it (or never confirms).
    property bool pending: false
    property bool _rbPower: false
    property string _rbMode: "off"
    property real _rbSetpoint: 25
    // Last authoritative values reported by the daemon.
    property bool _devPower: false
    property string _devMode: "off"
    property real _devSetpoint: 25
    property string _devFanSpeed: "auto"
    property bool _devSuper: false
    property bool _devSleep: false
    property bool _devFanMute: false
    property bool _devSwing: false
    property bool _devDimmer: false
    // Last non-off mode, so a power-on can show the resumed mode at once
    // instead of waiting for the next poll to confirm it.
    property string _lastMode: "cool"

    // ---- backend selection ------------------------------------------------
    readonly property string matterService: "org.kde.hisenseac"
    readonly property string cloudService: "org.kde.hisenseac.connectlife"
    readonly property string acPath: "/HisenseAC"
    readonly property string matterIface: "org.kde.hisenseac.Control"
    readonly property string cloudIface: "org.kde.hisenseac.ConnectLife"

    readonly property string configuredBackend: {
        const b = plasmoid.configuration.backend
        return (b === "matter" || b === "cloud") ? b : "matter"
    }
    readonly property bool matterAvailable: matterWatcher.registered
    readonly property bool cloudAvailable: cloudWatcher.registered
    readonly property string backendActive:
        configuredBackend === "cloud" ? "cloud" : "matter"
    readonly property bool isCloud: backendActive === "cloud"
    readonly property bool isMatter: !isCloud
    // Whether the currently selected daemon should be kept running.
    readonly property bool selectedDaemonEnabled: isCloud
        ? Boolean(plasmoid.configuration.cloudEnabled)
        : Boolean(plasmoid.configuration.matterEnabled)
    // Dry and fan modes have no temperature target, so the setpoint arc and the
    // +/- buttons are hidden there.
    readonly property bool tempAdjustable: mode !== "dry" && mode !== "fan"
    // Dry mode runs the fan at a fixed speed; fan speed is shown but locked.
    readonly property bool fanAdjustable: mode !== "dry"
    readonly property var activeProps: isCloud ? cloudProps : matterProps
    readonly property bool daemonAvailable: isCloud ? cloudAvailable : matterAvailable
    // Cloud needs no in-widget pairing (its credentials live in a config file),
    // so its main card is always available.
    readonly property bool setupComplete: isCloud ? true : commissioned

    function cap(s) {
        return s.length ? s.charAt(0).toUpperCase() + s.slice(1) : s
    }

    function fanLabel(s) {
        switch (s) {
        case "super_low": return qsTr("Super low")
        case "low": return qsTr("Low")
        case "medium": return qsTr("Medium")
        case "high": return qsTr("High")
        case "super_high": return qsTr("Super high")
        default: return qsTr("Auto")
        }
    }

    // coerces both the decoded value-type wrappers (returned by the initial
    // GetAll) and the raw values (from PropertiesChanged) to plain JS values.
    function _num(v, fallback) {
        const n = Number(v)
        return isNaN(n) ? fallback : n
    }

    // Applies the active daemon's exported state.
    function applyStatus() {
        const p = activeProps.properties
        // An unset key means we have not received a snapshot yet.
        if (p === undefined)
            return

        if (isMatter) {
            if (p.Connected === undefined)
                return
            connected = Boolean(p.Connected)
            commissioned = Boolean(p.Commissioned)
            minSetpoint = _num(p.MinSetpoint, 16)
            maxSetpoint = _num(p.MaxSetpoint, 32)
            lastError = p.LastError === undefined ? "" : String(p.LastError)
            provisioning = Boolean(p.Provisioning)
            unpairing = Boolean(p.Unpairing)
            provisionError = p.ProvisionError === undefined ? "" : String(p.ProvisionError)

            // Once the daemon has finished (commissioned the AC or reported an
            // error) drop the optimistic "pairing" state.
            if (pairingRequested && !provisioning
                    && (commissioned || provisionError.length > 0)) {
                pairingRequested = false
                pairStallTimer.stop()
            }

            if (forgetting) {
                if (unpairing) {
                    _sawUnpairing = true
                } else if (_sawUnpairing || !commissioned
                           || provisionError.length > 0) {
                    forgetting = false
                    _sawUnpairing = false
                    forgetTimer.stop()
                }
            }

            _devPower = Boolean(p.Power)
            _devMode = p.Mode === undefined ? "off" : String(p.Mode)
            _devSetpoint = _num(p.CoolingSetpoint, 0)
            localTemp = _num(p.LocalTemperature, 0)
        } else {
            if (p.Connected === undefined && p.Power === undefined)
                return
            connected = Boolean(p.Connected)
            commissioned = true
            minSetpoint = 16
            maxSetpoint = 32
            lastError = p.LastError === undefined ? "" : String(p.LastError)
            deviceId = p.DeviceId === undefined ? "" : String(p.DeviceId)

            _devPower = Boolean(p.Power)
            _devMode = p.Mode === undefined ? "off" : String(p.Mode)
            _devSetpoint = _num(p.Temperature, 0)
            localTemp = _num(p.LocalTemperature, 0)
            _devFanSpeed = p.FanSpeed === undefined ? "auto" : String(p.FanSpeed)
            _devSuper = Boolean(p.Super)
            _devSleep = Boolean(p.Sleep)
            _devFanMute = Boolean(p.FanMute)
            _devSwing = Boolean(p.Swing)
            _devDimmer = Boolean(p.Dimmer)
            humidity = _num(p.Humidity, 0)
        }

        if (_devMode !== "off")
            _lastMode = _devMode

        if (pending) {
            if (lastError.length > 0) {
                rollback()
            } else if (_devPower === power && _devMode === mode
                       && Math.abs(_devSetpoint - setpoint) < 0.5) {
                // Device confirmed the change.
                pending = false
                pendingTimer.stop()
                power = _devPower
                mode = _devMode
                setpoint = _devSetpoint
            }
            // Otherwise keep showing the optimistic value.
        } else {
            power = _devPower
            mode = _devMode
            setpoint = _devSetpoint
        }

        if (isCloud && !extrasLock) {
            fanSpeed = _devFanSpeed
            superFast = _devSuper
            sleep = _devSleep
            fanMute = _devFanMute
            swing = _devSwing
            dimmer = _devDimmer
        }
    }

    // Force a fresh snapshot from the active daemon (used as a safety net).
    function refresh() {
        activeProps.updateAll()
    }

    // Fire-and-forget asynchronous D-Bus method call against the active daemon.
    // Method success is never taken as authoritative; the resulting state
    // arrives via PropertiesChanged.
    function _call(member, args, signature, onError) {
        const service = isCloud ? cloudService : matterService
        const iface = isCloud ? cloudIface : matterIface
        // A D-Bus message signature is a type list, e.g. "(d)" or "(uu)" -- the
        // surrounding parentheses are required. The DBusMessage map constructor
        // also expects the key "interface" (the QML property is exposed as
        // "iface", but the map is read by name on the C++ side).
        const fullSignature = signature && signature.length > 0 ? "(" + signature + ")" : ""
        DBus.SessionBus.asyncCall({
            service: service,
            path: root.acPath,
            iface: iface,
            member: member,
            signature: fullSignature,
            arguments: args
        }, function () {
        }, function (reply) {
            const message = (reply && reply.error && reply.error.message)
                ? reply.error.message : ""
            if (onError)
                onError(message)
        })
    }

    function _beginPending() {
        if (!pending) {
            _rbPower = power
            _rbMode = mode
            _rbSetpoint = setpoint
        }
        pending = true
        pendingTimer.restart()
    }

    function rollback() {
        power = _rbPower
        mode = _rbMode
        setpoint = _rbSetpoint
        pending = false
        pendingTimer.stop()
    }

    function setSetpoint(v) {
        v = Math.round(Math.max(minSetpoint, Math.min(maxSetpoint, v)))
        if (v === Math.round(setpoint))
            return
        _beginPending()
        setpoint = v
        const member = isCloud ? "SetTemperature" : "SetSetpoint"
        _call(member, [v], "d", function (m) { root.lastError = m; root.rollback() })
    }

    function setMode(m) {
        if (m === mode)
            return
        _beginPending()
        mode = m
        power = true
        _call("SetMode", [m], "s", function (msg) { root.lastError = msg; root.rollback() })
    }

    function setPower(on) {
        if (on === power)
            return
        _beginPending()
        power = on
        // Resuming from off: show the mode the unit will come back up in
        // immediately, instead of waiting for the next poll to confirm it.
        if (on && _lastMode !== "off")
            mode = _lastMode
        _call("SetPower", [on], "b", function (m) { root.lastError = m; root.rollback() })
    }

    // ---- cloud-only controls ---------------------------------------------
    function _pushExtra(member, args, signature) {
        if (!isCloud)
            return
        extrasLock = true
        extrasTimer.restart()
        _call(member, args, signature, function (m) { root.lastError = m })
    }

    function setFanSpeed(s) {
        if (!isCloud || s === fanSpeed)
            return
        fanSpeed = s
        _pushExtra("SetFanSpeed", [s], "s")
    }

    function toggleSleep() {
        sleep = !sleep
        _pushExtra("SetSleep", [sleep], "b")
    }

    function toggleSuper() {
        superFast = !superFast
        _pushExtra("SetSuper", [superFast], "b")
    }

    function toggleSwing() {
        swing = !swing
        _pushExtra("SetSwing", [swing], "b")
    }

    function toggleDimmer() {
        dimmer = !dimmer
        _pushExtra("SetDimmer", [dimmer], "b")
    }

    function provision(ssid, password, code) {
        provError = ""
        pairingRequested = true
        pairStallTimer.restart()
        _call("Provision", [ssid, password, code], "sss", function () {
            provError = qsTr("Could not reach the daemon (is hisense-matterd running?)")
            pairingRequested = false
            pairStallTimer.stop()
        })
    }

    function forget() {
        provError = ""
        confirmForget = false
        forgetting = true
        _sawUnpairing = false
        forgetTimer.restart()
        _call("Unpair", [], "", function () {
            provError = qsTr("Could not reach the daemon (is hisense-matterd running?)")
            forgetting = false
            forgetTimer.stop()
        })
    }

    // ---- daemon bindings --------------------------------------------------
    // Event-driven status: each daemon publishes PropertiesChanged whenever its
    // state changes. Only the active one's snapshot is read by applyStatus().
    DBus.Properties {
        id: matterProps
        service: root.matterService
        path: root.acPath
        iface: root.matterIface
        onPropertiesChanged: root.applyStatus()
    }
    DBus.Properties {
        id: cloudProps
        service: root.cloudService
        path: root.acPath
        iface: root.cloudIface
        onPropertiesChanged: root.applyStatus()
    }
    DBus.DBusServiceWatcher {
        id: matterWatcher
        watchedService: root.matterService
        onRegisteredChanged: if (registered && root.isMatter) root.applyStatus()
    }
    DBus.DBusServiceWatcher {
        id: cloudWatcher
        watchedService: root.cloudService
        onRegisteredChanged: if (registered && root.isCloud) root.applyStatus()
    }

    // ---- systemd daemon policy -------------------------------------------
    // Only the selected daemon runs. Selecting a backend makes the widget
    // start+enable that daemon and stop+disable the other, so the choice
    // sticks across reboots. Toggling "Run daemon" off stops+disables it.
    readonly property string matterUnit: "hisense-matterd.service"
    readonly property string cloudUnit: "hisense-connectlife.service"
    readonly property string sysdService: "org.freedesktop.systemd1"
    readonly property string sysdPath: "/org/freedesktop/systemd1"
    readonly property string sysdIface: "org.freedesktop.systemd1.Manager"

    function sysdCall(member, args, signature) {
        DBus.SessionBus.asyncCall({
            service: root.sysdService,
            path: root.sysdPath,
            iface: root.sysdIface,
            member: member,
            signature: signature && signature.length > 0 ? "(" + signature + ")" : "",
            arguments: args
        }, function () {
        }, function () {
        })
    }

    // Bring one unit to the desired state (running+enabled, or stopped+disabled).
    function setDaemon(unit, on) {
        if (on) {
            sysdCall("EnableUnitFiles", [[unit], false, true], "asbb")
            sysdCall("StartUnit", [unit, "replace"], "ss")
        } else {
            sysdCall("StopUnit", [unit, "replace"], "ss")
            sysdCall("DisableUnitFiles", [[unit], false], "asb")
        }
    }

    function applyDaemonPolicy() {
        const selected = root.isCloud ? root.cloudUnit : root.matterUnit
        const other = root.isCloud ? root.matterUnit : root.cloudUnit
        setDaemon(selected, root.selectedDaemonEnabled)
        setDaemon(other, false)
    }
    onBackendActiveChanged: Qt.callLater(function () { root.refresh() })

    // Pull an initial snapshot and re-pull whenever the active backend changes.
    Component.onCompleted: {
        root.refresh()
        Qt.callLater(root.applyDaemonPolicy)
    }
    Connections {
        target: plasmoid.configuration
        function onValueChanged(key, value) {
            if (key === "backend" || key === "matterEnabled" || key === "cloudEnabled") {
                root.applyDaemonPolicy()
            }
        }
    }

    // ---- mode menu --------------------------------------------------------
    PlasmaComponents.Menu {
        id: modeMenu
        onVisibleChanged: if (visible) syncChecks()
        function syncChecks() {
            autoItem.checked = root.mode === "auto"
            coolItem.checked = root.mode === "cool"
            dryItem.checked = root.mode === "dry"
            fanItem.checked = root.mode === "fan"
            heatItem.checked = root.mode === "heat"
        }
        PlasmaComponents.MenuItem {
            id: autoItem
            visible: root.isCloud
            text: qsTr("Auto")
            checkable: true
            autoExclusive: true
            onTriggered: root.setMode("auto")
        }
        PlasmaComponents.MenuItem {
            id: coolItem
            text: qsTr("Cool")
            checkable: true
            autoExclusive: true
            onTriggered: root.setMode("cool")
        }
        PlasmaComponents.MenuItem {
            id: dryItem
            visible: root.isCloud
            text: qsTr("Dry")
            checkable: true
            autoExclusive: true
            onTriggered: root.setMode("dry")
        }
        PlasmaComponents.MenuItem {
            id: fanItem
            visible: root.isCloud
            text: qsTr("Fan only")
            checkable: true
            autoExclusive: true
            onTriggered: root.setMode("fan")
        }
        PlasmaComponents.MenuItem {
            id: heatItem
            text: qsTr("Heat")
            checkable: true
            autoExclusive: true
            onTriggered: root.setMode("heat")
        }
    }

    // ---- fan speed menu (cloud only) -------------------------------------
    PlasmaComponents.Menu {
        id: fanMenu
        onVisibleChanged: if (visible) syncChecks()
        function syncChecks() {
            for (let i = 0; i < count; ++i) {
                const it = itemAt(i)
                it.checked = (it.value === root.fanSpeed)
            }
        }
        Repeater {
            model: ["auto", "super_low", "low", "medium", "high", "super_high"]
            PlasmaComponents.MenuItem {
                property string value: modelData
                text: root.fanLabel(modelData)
                checkable: true
                autoExclusive: true
                onTriggered: root.setFanSpeed(value)
            }
        }
    }

    // ---- timers -----------------------------------------------------------
    Timer {
        id: confirmTimer
        interval: 4000
        repeat: false
        onTriggered: root.confirmForget = false
    }
    Timer {
        id: forgetTimer
        interval: 30000
        repeat: false
        onTriggered: {
            root.forgetting = false
            root._sawUnpairing = false
            root.refresh()
        }
    }
    Timer {
        id: pendingTimer
        interval: 9000
        repeat: false
        onTriggered: {
            root.pending = false
            root.power = root._devPower
            root.mode = root._devMode
            root.setpoint = root._devSetpoint
        }
    }
    Timer {
        id: extrasTimer
        interval: 8000
        repeat: false
        onTriggered: {
            root.extrasLock = false
            root.refresh()
        }
    }
    Timer {
        id: pairStallTimer
        interval: 180000
        repeat: false
        onTriggered: {
            root.pairingRequested = false
            root.refresh()
        }
    }

    // ---- representations --------------------------------------------------
    compactRepresentation: Item {
        id: compact
        readonly property int hPadding: Kirigami.Units.smallSpacing * 3
        implicitWidth: compactRow.implicitWidth + hPadding * 2
        implicitHeight: Math.max(compactRow.implicitHeight, Kirigami.Units.iconSizes.small)
        Layout.minimumWidth: implicitWidth
        Layout.preferredWidth: implicitWidth
        Layout.fillHeight: true
        RowLayout {
            id: compactRow
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing
            PlasmaComponents.Label {
                text: root.setupComplete ? root.localTemp.toFixed(0) + "\u00B0" : "\u2014"
                font.bold: true
            }
            PlasmaComponents.Label {
                text: !root.setupComplete ? qsTr("Not set up")
                    : (!root.connected ? qsTr("Offline")
                                       : (root.power ? root.cap(root.mode) : qsTr("Off")))
                opacity: 0.8
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: root.expanded = !root.expanded
        }
    }

    fullRepresentation: ColumnLayout {
        id: full
        spacing: Kirigami.Units.largeSpacing
        Layout.minimumWidth: 340
        Layout.preferredWidth: 360
        Layout.minimumHeight: 480
        Layout.preferredHeight: implicitHeight

        readonly property bool dark: Kirigami.Theme.backgroundColor.hslLightness < 0.5
        readonly property color accent: "#2f7de1"
        readonly property color cardColor: dark
            ? Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.05)
            : Qt.rgba(1, 1, 1, 1)
        readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r,
                                                  Kirigami.Theme.textColor.g,
                                                  Kirigami.Theme.textColor.b,
                                                  dark ? 0.16 : 0.09)

        // ---- offline hints ---------------------------------------------
        PlasmaComponents.Label {
            Layout.fillWidth: true
            visible: root.setupComplete && !root.daemonAvailable
            text: root.isCloud
                ? qsTr("\u26A0 ConnectLife daemon not running (hisense-connectlife) \u2014 enable it in settings or start it.")
                : qsTr("\u26A0 Matter daemon not running (hisense-matterd) \u2014 enable it in settings or start it.")
            color: "#e0a251"
            wrapMode: Text.WordWrap
            font.pointSize: 8
        }
        PlasmaComponents.Label {
            Layout.fillWidth: true
            visible: root.setupComplete && root.daemonAvailable && !root.connected
                     && root.provisionError.length === 0
            text: qsTr("\u26A0 Air conditioner offline \u2014 showing its last known state. Check power and Wi-Fi.")
            color: "#e0a251"
            wrapMode: Text.WordWrap
            font.pointSize: 8
        }

        // ---- setup (Matter only, until commissioned) -------------------
        Rectangle {
            visible: root.isMatter && !root.commissioned
            Layout.fillWidth: true
            Layout.preferredHeight: setupCol.implicitHeight + Kirigami.Units.largeSpacing * 2
            radius: Kirigami.Units.largeSpacing
            color: full.cardColor
            border.width: 1
            border.color: full.hairline

            ColumnLayout {
                id: setupCol
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing

                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    text: qsTr("Connect the air conditioner")
                    font.bold: true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    PlasmaComponents.Label {
                        Layout.fillWidth: true
                        text: qsTr("1. Put the AC in pairing mode: hold its Wi-Fi button ~3 s until the Wi-Fi icon blinks.")
                        opacity: 0.6
                        font.pointSize: 8
                        wrapMode: Text.WordWrap
                    }
                    PlasmaComponents.Label {
                        Layout.fillWidth: true
                        text: qsTr("2. Use a 2.4 GHz Wi-Fi network.")
                        opacity: 0.6
                        font.pointSize: 8
                        wrapMode: Text.WordWrap
                    }
                    PlasmaComponents.Label {
                        Layout.fillWidth: true
                        text: qsTr("3. Enter the Wi-Fi name and password, then the pairing code (on the unit or its QR card).")
                        opacity: 0.6
                        font.pointSize: 8
                        wrapMode: Text.WordWrap
                    }
                }
                PlasmaComponents.TextField {
                    id: ssidField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Wi-Fi name (SSID)")
                }
                PlasmaComponents.TextField {
                    id: passField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Wi-Fi password")
                    echoMode: TextInput.Password
                }
                PlasmaComponents.TextField {
                    id: codeField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Pairing code")
                    inputMethodHints: Qt.ImhDigitsOnly
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    PlasmaComponents.BusyIndicator {
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        running: root.provisioning || root.pairingRequested
                        visible: running
                    }
                    PlasmaComponents.Button {
                        Layout.fillWidth: true
                        text: (root.provisioning || root.pairingRequested)
                              ? qsTr("Pairing\u2026") : qsTr("Pair")
                        enabled: !(root.provisioning || root.pairingRequested)
                                 && ssidField.text.length > 0
                                 && passField.text.length > 0 && codeField.text.length > 0
                        onClicked: root.provision(ssidField.text, passField.text, codeField.text)
                    }
                }
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.provError.length > 0 ? root.provError : root.provisionError
                    color: "#e05151"
                    wrapMode: Text.Wrap
                    font.pointSize: 8
                }
            }
        }

        // ---- main card -------------------------------------------------
        Rectangle {
            visible: root.setupComplete
            Layout.fillWidth: true
            Layout.preferredHeight: mainCol.implicitHeight + Kirigami.Units.largeSpacing * 2
            radius: Kirigami.Units.largeSpacing
            color: full.cardColor
            border.width: 1
            border.color: full.hairline

            ColumnLayout {
                id: mainCol
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    PlasmaComponents.Label {
                        text: qsTr("Indoor")
                        opacity: 0.6
                    }
                    PlasmaComponents.Label {
                        text: root.localTemp.toFixed(0) + "\u00B0C"
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    CircleButton {
                        iconName: "system-shutdown"
                        emphasized: root.power
                        accentColor: full.accent
                        enabled: root.connected
                        onClicked: root.setPower(!root.power)
                    }
                }

                Item {
                    // This area is always reserved so the widget keeps the same
                    // height in every mode. In dry and fan-only modes (which have
                    // no temperature target) a mode icon/description replaces the
                    // setpoint arc instead of leaving a gap.
                    Layout.fillWidth: true
                    Layout.preferredHeight: 190

                    TempArc {
                        id: arc
                        visible: root.tempAdjustable
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: height
                        height: parent.height
                        from: root.minSetpoint
                        to: root.maxSetpoint
                        value: root.setpoint
                        stepSize: 1
                        activeColor: full.accent
                        inactiveColor: full.dark
                            ? Qt.rgba(full.accent.r, full.accent.g, full.accent.b, 0.22)
                            : "#d5e2f7"
                        enabled: root.connected && root.power
                        opacity: enabled ? 1 : 0.5
                        onMoved: (value) => root.setSetpoint(value)
                    }

                    ColumnLayout {
                        anchors.centerIn: parent
                        visible: root.tempAdjustable
                        spacing: 0
                        PlasmaComponents.Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("TEMP")
                            opacity: 0.5
                            font.pointSize: 8
                            font.letterSpacing: 1.5
                        }
                        PlasmaComponents.Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: Math.round(arc.displayValue).toString()
                            font.pointSize: 40
                            font.bold: true
                        }
                        PlasmaComponents.Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "\u00B0C"
                            opacity: 0.6
                            font.pointSize: 11
                        }
                    }

                    CircleButton {
                        visible: root.tempAdjustable
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "list-remove"
                        diameter: Kirigami.Units.gridUnit * 2
                        accentColor: full.accent
                        enabled: root.connected && root.power
                        onClicked: root.setSetpoint(root.setpoint - 1)
                    }
                    CircleButton {
                        visible: root.tempAdjustable
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "list-add"
                        diameter: Kirigami.Units.gridUnit * 2
                        accentColor: full.accent
                        enabled: root.connected && root.power
                        onClicked: root.setSetpoint(root.setpoint + 1)
                    }

                    // Placeholder shown instead of the arc in dry / fan-only mode.
                    ColumnLayout {
                        anchors.centerIn: parent
                        visible: !root.tempAdjustable
                        width: parent.width
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Icon {
                            Layout.alignment: Qt.AlignHCenter
                            source: root.mode === "fan" ? "fan-symbolic" : "raindrop"
                            implicitWidth: Kirigami.Units.iconSizes.huge
                            implicitHeight: Kirigami.Units.iconSizes.huge
                            opacity: 0.55
                        }
                        PlasmaComponents.Label {
                            Layout.fillWidth: true
                            text: root.mode === "fan" ? qsTr("Fan only") : qsTr("Dry")
                            horizontalAlignment: Text.AlignHCenter
                            font.bold: true
                            font.pointSize: 13
                        }
                        PlasmaComponents.Label {
                            Layout.fillWidth: true
                            text: root.mode === "fan"
                                ? qsTr("No temperature control")
                                : qsTr("Dehumidifying at a fixed fan speed")
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            opacity: 0.6
                            font.pointSize: 8
                        }
                    }
                }

                // mode / speed selectors
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Rectangle {
                        id: modeBox
                        Layout.fillWidth: true
                        implicitHeight: modeRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                        radius: Kirigami.Units.smallSpacing
                        color: "transparent"
                        border.width: 1
                        border.color: full.hairline

                        RowLayout {
                            id: modeRow
                            anchors.fill: parent
                            anchors.leftMargin: Kirigami.Units.smallSpacing * 1.5
                            anchors.rightMargin: Kirigami.Units.smallSpacing * 1.5
                            spacing: Kirigami.Units.smallSpacing
                            PlasmaComponents.Label {
                                text: qsTr("Mode")
                                opacity: 0.5
                                font.pointSize: 9
                            }
                            Item { Layout.fillWidth: true }
                            PlasmaComponents.Label {
                                text: root.power ? root.cap(root.mode) : qsTr("Off")
                                font.bold: true
                            }
                            Kirigami.Icon {
                                source: "arrow-down"
                                implicitWidth: Kirigami.Units.iconSizes.small
                                implicitHeight: Kirigami.Units.iconSizes.small
                                opacity: 0.6
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: root.connected
                            cursorShape: Qt.PointingHandCursor
                            onClicked: modeMenu.popup(modeBox, 0, modeBox.height)
                        }
                    }

                    Rectangle {
                        id: speedBox
                        Layout.fillWidth: true
                        implicitHeight: modeRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                        radius: Kirigami.Units.smallSpacing
                        color: "transparent"
                        border.width: 1
                        border.color: full.hairline
                        opacity: (root.isCloud && root.fanAdjustable) ? 1 : 0.55
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Kirigami.Units.smallSpacing * 1.5
                            anchors.rightMargin: Kirigami.Units.smallSpacing * 1.5
                            spacing: Kirigami.Units.smallSpacing
                            PlasmaComponents.Label {
                                text: qsTr("Speed")
                                opacity: 0.6
                                font.pointSize: 9
                            }
                            Item { Layout.fillWidth: true }
                            PlasmaComponents.Label {
                                text: root.isCloud ? root.fanLabel(root.fanSpeed) : qsTr("N/A")
                                font.bold: true
                            }
                            Kirigami.Icon {
                                visible: root.isCloud
                                source: "arrow-down"
                                implicitWidth: Kirigami.Units.iconSizes.small
                                implicitHeight: Kirigami.Units.iconSizes.small
                                opacity: 0.6
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: root.isCloud && root.connected && root.fanAdjustable
                            cursorShape: Qt.PointingHandCursor
                            onClicked: fanMenu.popup(speedBox, 0, speedBox.height)
                        }
                    }
                }
            }
        }

        // ---- quick actions ----
        Rectangle {
            visible: root.setupComplete
            Layout.fillWidth: true
            Layout.preferredHeight: quickCol.implicitHeight + Kirigami.Units.largeSpacing * 2
            radius: Kirigami.Units.largeSpacing
            color: full.cardColor
            border.width: 1
            border.color: full.hairline

            ColumnLayout {
                id: quickCol
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: Kirigami.Units.largeSpacing

                    ColumnLayout {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 3.4
                        spacing: Kirigami.Units.smallSpacing
                        CircleButton {
                            Layout.alignment: Qt.AlignHCenter
                            iconName: "media-playlist-repeat"
                            diameter: Kirigami.Units.gridUnit * 2.4
                            accentColor: full.accent
                            emphasized: root.swing
                            enabled: root.isCloud && root.connected && root.power
                            onClicked: root.toggleSwing()
                        }
                        PlasmaComponents.Label {
                            Layout.fillWidth: true
                            text: qsTr("Swing")
                            horizontalAlignment: Text.AlignHCenter
                            opacity: root.isCloud ? 0.8 : 0.4
                        }
                    }

                    ColumnLayout {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 3.4
                        spacing: Kirigami.Units.smallSpacing
                        CircleButton {
                            Layout.alignment: Qt.AlignHCenter
                            iconName: "weather-clear-night"
                            diameter: Kirigami.Units.gridUnit * 2.4
                            accentColor: full.accent
                            emphasized: root.sleep
                            enabled: root.isCloud && root.connected && root.power
                            onClicked: root.toggleSleep()
                        }
                        PlasmaComponents.Label {
                            Layout.fillWidth: true
                            text: qsTr("Sleep")
                            horizontalAlignment: Text.AlignHCenter
                            opacity: root.isCloud ? 0.8 : 0.4
                        }
                    }

                    ColumnLayout {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 3.4
                        spacing: Kirigami.Units.smallSpacing
                        CircleButton {
                            Layout.alignment: Qt.AlignHCenter
                            iconName: "weather-snow"
                            diameter: Kirigami.Units.gridUnit * 2.4
                            accentColor: full.accent
                            emphasized: root.superFast
                            enabled: root.isCloud && root.connected && root.power
                            onClicked: root.toggleSuper()
                        }
                        PlasmaComponents.Label {
                            Layout.fillWidth: true
                            text: qsTr("Fast cooling")
                            horizontalAlignment: Text.AlignHCenter
                            opacity: root.isCloud ? 0.8 : 0.4
                        }
                    }

                    ColumnLayout {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 3.4
                        spacing: Kirigami.Units.smallSpacing
                        CircleButton {
                            Layout.alignment: Qt.AlignHCenter
                            iconName: "games-hint"
                            diameter: Kirigami.Units.gridUnit * 2.4
                            accentColor: full.accent
                            emphasized: root.dimmer
                            enabled: root.isCloud && root.connected && root.power
                            onClicked: root.toggleDimmer()
                        }
                        PlasmaComponents.Label {
                            Layout.fillWidth: true
                            text: qsTr("Display")
                            horizontalAlignment: Text.AlignHCenter
                            opacity: root.isCloud ? 0.8 : 0.4
                        }
                    }

                }

                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    visible: !root.isCloud
                    text: qsTr("These controls need the ConnectLife cloud daemon.")
                    horizontalAlignment: Text.AlignHCenter
                    opacity: 0.5
                    font.pointSize: 8
                }
            }
        }

        PlasmaComponents.Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.connected ? root.lastError
                : (root.provisionError.length > 0 ? root.provisionError : root.lastError)
            color: "#e05151"
            wrapMode: Text.WordWrap
            font.pointSize: 8
        }

        // ---- forget action (Matter pairing only) ----
        RowLayout {
            Layout.fillWidth: true
            visible: root.connected && root.isMatter
            spacing: Kirigami.Units.smallSpacing
            PlasmaComponents.BusyIndicator {
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                running: root.unpairing || root.forgetting
                visible: running
            }
            PlasmaComponents.Button {
                Layout.fillWidth: true
                flat: true
                icon.name: "edit-delete"
                text: (root.unpairing || root.forgetting) ? qsTr("Forgetting\u2026")
                    : (root.confirmForget ? qsTr("Click again to remove the pairing")
                                          : qsTr("Forget air conditioner"))
                enabled: !root.unpairing && !root.forgetting && !root.provisioning
                onClicked: {
                    if (root.confirmForget) {
                        root.forget()
                    } else {
                        root.confirmForget = true
                        confirmTimer.restart()
                    }
                }
            }
        }
    }

    toolTipMainText: "Hisense AC"
    toolTipSubText: root.connected
        ? (root.localTemp.toFixed(1) + " \u00B0C, " + (root.power ? root.cap(root.mode) : "Off"))
        : (root.setupComplete ? qsTr("Offline") : qsTr("Not paired"))
}
