// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC

import org.kde.plasma.plasmoid
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.plasma5support as Plasma5Support

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

    function cap(s) {
        return s.length ? s.charAt(0).toUpperCase() + s.slice(1) : s
    }

    readonly property string acCommand: Plasmoid.configuration.command
    // plasmashell's PATH usually lacks ~/.local/bin, but the executable engine
    // runs commands through a shell, so we can prepend it inline.
    readonly property string shellPrefix: "PATH=\"$HOME/.local/bin:$PATH\" "
    readonly property string statusCommand: shellPrefix + acCommand + " status --json"

    function applyStatus(text) {
        try {
            const s = JSON.parse(text)
            connected = s.connected
            commissioned = s.commissioned === true
            localTemp = s.localTemperature
            minSetpoint = s.minSetpoint
            maxSetpoint = s.maxSetpoint
            lastError = s.lastError
            provisioning = s.provisioning === true
            unpairing = s.unpairing === true
            provisionError = s.provisionError || ""

            // Once the daemon has finished (commissioned the AC or reported an
            // error) drop the optimistic "pairing" state.
            if (pairingRequested && !provisioning
                    && (commissioned || provisionError.length > 0)) {
                pairingRequested = false
                pairStallTimer.stop()
            }

            if (forgetting) {
                if (unpairing)
                    _sawUnpairing = true
                else if (_sawUnpairing || !commissioned) {
                    // Unpair finished (either way) or the fabric is gone.
                    forgetting = false
                    _sawUnpairing = false
                    forgetTimer.stop()
                }
            }

            _devPower = s.power
            _devMode = s.mode
            _devSetpoint = s.coolingSetpoint

            if (pending) {
                if (lastError.length > 0) {
                    rollback()
                } else if (s.power === power && s.mode === mode
                           && Math.abs(s.coolingSetpoint - setpoint) < 0.5) {
                    // Device confirmed the change.
                    pending = false
                    pendingTimer.stop()
                    power = s.power
                    mode = s.mode
                    setpoint = s.coolingSetpoint
                }
                // Otherwise keep showing the optimistic value.
            } else {
                power = s.power
                mode = s.mode
                setpoint = s.coolingSetpoint
            }
        } catch (e) {
            // ignore malformed output (daemon may be starting up)
        }
    }

    function refresh() {
        statusSource.disconnectSource(statusCommand)
        statusSource.connectSource(statusCommand)
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
        send("setpoint " + v)
    }

    function setMode(key) {
        if (mode === key)
            return
        _beginPending()
        mode = key
        if (key !== "off")
            power = true
        send("mode " + key)
    }

    function setPower(on) {
        if (power === on)
            return
        _beginPending()
        power = on
        send(on ? "on" : "off")
    }

    // Single-quote a value for the shell the executable engine runs commands in.
    function _shq(s) {
        return "'" + String(s).replace(/'/g, "'\\''") + "'"
    }

    function provision(ssid, password, code) {
        if (ssid.length === 0 || password.length === 0 || code.length === 0) {
            provError = qsTr("Enter the Wi-Fi name, password and pairing code.")
            return
        }
        provError = ""
        pairingRequested = true
        pairStallTimer.restart()
        cmdSource.run(root.shellPrefix + root.acCommand + " provision "
                      + _shq(ssid) + " " + _shq(password) + " " + _shq(code), ok => {
            if (!ok) {
                provError = qsTr("Could not reach the daemon (is hisense-matterd running?)")
                pairingRequested = false
                pairStallTimer.stop()
            }
        })
        refreshTimer.remaining = 10
        refreshTimer.restart()
    }

    function forget() {
        provError = ""
        confirmForget = false
        forgetting = true
        _sawUnpairing = false
        forgetTimer.restart()
        cmdSource.run(root.shellPrefix + root.acCommand + " unpair", ok => {
            if (!ok) {
                provError = qsTr("Could not reach the daemon (is hisense-matterd running?)")
                forgetting = false
                forgetTimer.stop()
            }
        })
        refreshTimer.remaining = 10
        refreshTimer.restart()
    }

    Plasma5Support.DataSource {
        id: statusSource
        engine: "executable"
        connectedSources: [root.statusCommand]
        onNewData: (source, data) => {
            if (data["exit code"] === 0 && data.stdout)
                root.applyStatus(data.stdout)
        }
    }

    // Always poll the daemon so the UI tracks it even when no command was
    // issued (the DataSource itself only runs once per connect).
    Timer {
        id: pollTimer
        interval: 10000
        repeat: true
        running: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    // One-shot command runner: connect, collect, disconnect.
    Plasma5Support.DataSource {
        id: cmdSource
        engine: "executable"
        connectedSources: []
        property var onDone: null
        onNewData: (source, data) => {
            const ok = data["exit code"] === 0
            const done = onDone
            onDone = null
            disconnectSource(source)
            if (done)
                done(ok)
        }
        function run(cmd, done) { onDone = done; connectSource(cmd) }
    }

    // Mode selector popup. autoExclusive renders radio (single-choice) markers.
    // Only Cool and Heat are honoured by this unit's firmware (thermostat
    // FeatureMap = 3); Auto/Fan/Dry are accepted over Matter but ignored by the
    // hardware, so they are intentionally not offered.
    PlasmaComponents.Menu {
        id: modeMenu
        // autoExclusive rewrites `checked` on click, so re-assert on each open.
        onVisibleChanged: if (visible) syncChecks()
        function syncChecks() {
            coolItem.checked = root.mode === "cool"
            heatItem.checked = root.mode === "heat"
        }
        PlasmaComponents.MenuItem {
            id: coolItem
            text: qsTr("Cool")
            checkable: true
            autoExclusive: true
            onTriggered: root.setMode("cool")
        }
        PlasmaComponents.MenuItem {
            id: heatItem
            text: qsTr("Heat")
            checkable: true
            autoExclusive: true
            onTriggered: root.setMode("heat")
        }
    }

    // Poll the daemon briskly after a command so the optimistic value is
    // confirmed (or rolled back) quickly.
    Timer {
        id: refreshTimer
        interval: 800
        repeat: true
        property int remaining: 0
        onTriggered: {
            root.refresh()
            if (--remaining <= 0)
                stop()
        }
    }

    // Lapse the "really forget?" confirmation state after a few seconds.
    Timer {
        id: confirmTimer
        interval: 4000
        repeat: false
        onTriggered: root.confirmForget = false
    }

    // Safety net so the button never stays stuck on "Forgetting…".
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

    // If the device never confirms the optimistic change, settle on whatever
    // the device actually reports (a rollback).
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

    function send(cmd) {
        cmdSource.run(root.shellPrefix + root.acCommand + " " + cmd, ok => {
            if (!ok)
                root.rollback()
        })
        refreshTimer.remaining = 10
        refreshTimer.restart()
    }

    // Keep polling while the daemon reports a pairing in progress (it can take
    // a minute or two).
    Timer {
        id: provisionPoller
        interval: 1000
        repeat: true
        running: root.provisioning || root.pairingRequested
        onTriggered: root.refresh()
    }

    // Safety net so the Pair button never stays stuck on "Pairing…".
    Timer {
        id: pairStallTimer
        interval: 180000
        repeat: false
        onTriggered: {
            root.pairingRequested = false
            root.refresh()
        }
    }

    // ---- representations ----
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
                text: root.commissioned ? root.localTemp.toFixed(0) + "\u00B0" : "\u2014"
                font.bold: true
            }
            PlasmaComponents.Label {
                text: !root.commissioned ? qsTr("Not set up")
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
        // Keep enough room that the arc and the -/+ buttons never overlap.
        Layout.minimumWidth: 340
        Layout.preferredWidth: 360
        // Size to content so there is no dead space under the quick actions.
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

        // ---- offline hint (paired, but the AC is not answering) --------
        PlasmaComponents.Label {
            Layout.fillWidth: true
            visible: root.commissioned && !root.connected && root.provisionError.length === 0
            text: qsTr("\u26A0 Air conditioner offline \u2014 showing its last known state. Check power and Wi-Fi.")
            color: "#e0a251"
            wrapMode: Text.WordWrap
            font.pointSize: 8
        }

        // ---- setup (shown until the AC has been commissioned) ----------
        Rectangle {
            visible: !root.commissioned
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
            visible: root.commissioned
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

                // header: indoor temperature + power
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

                // temperature arc + fine buttons + readout
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 190

                    TempArc {
                        id: arc
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
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "list-remove"
                        diameter: Kirigami.Units.gridUnit * 2
                        accentColor: full.accent
                        enabled: root.connected && root.power
                        onClicked: root.setSetpoint(root.setpoint - 1)
                    }
                    CircleButton {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "list-add"
                        diameter: Kirigami.Units.gridUnit * 2
                        accentColor: full.accent
                        enabled: root.connected && root.power
                        onClicked: root.setSetpoint(root.setpoint + 1)
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

                    // Speed is not exposed over Matter on this unit.
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: modeRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                        radius: Kirigami.Units.smallSpacing
                        color: "transparent"
                        border.width: 1
                        border.color: full.hairline
                        opacity: 0.55
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
                                text: qsTr("N/A")
                                font.bold: true
                            }
                        }
                    }
                }
            }
        }

        // ---- quick actions ----
        Rectangle {
            visible: root.commissioned
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
                    Repeater {
                        model: [
                            { icon: "media-playlist-repeat", label: qsTr("Swing") },
                            { icon: "weather-clear-night", label: qsTr("Sleep") },
                            { icon: "weather-snow", label: qsTr("Fast Cooling") },
                            { icon: "brightness-high", label: qsTr("Dimmer") }
                        ]
                        delegate: ColumnLayout {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 3.4
                            spacing: Kirigami.Units.smallSpacing
                            CircleButton {
                                Layout.alignment: Qt.AlignHCenter
                                iconName: modelData.icon
                                diameter: Kirigami.Units.gridUnit * 2.4
                                enabled: false
                            }
                            PlasmaComponents.Label {
                                Layout.fillWidth: true
                                text: modelData.label
                                font.pointSize: 8
                                opacity: 0.6
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                }

                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    text: qsTr("Not exposed over Matter on this unit")
                    opacity: 0.5
                    font.pointSize: 8
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        PlasmaComponents.Label {
            visible: text.length > 0
            text: (root.connected && root.lastError.length > 0)
                ? root.lastError
                : (root.commissioned ? root.provisionError : "")
            color: "#e05151"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.connected
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
        : (root.commissioned ? qsTr("Offline") : qsTr("Not paired"))
}
