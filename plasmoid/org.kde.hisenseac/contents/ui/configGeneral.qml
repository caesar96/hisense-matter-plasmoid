import QtQuick
import QtQuick.Controls as QQC
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: page

    // Values: "matter" | "cloud". The selected daemon is the one the widget
    // talks to and keeps running; the other is stopped and disabled.
    property string backend: "matter"
    property alias cfg_backend: page.backend
    property bool matterEnabled: false
    property alias cfg_matterEnabled: page.matterEnabled
    property bool cloudEnabled: false
    property alias cfg_cloudEnabled: page.cloudEnabled

    // Ensure exactly the selected backend is marked "run": selecting a backend
    // in the combobox enables it and disables the other one.
    function runSelected() {
        if (page.backend === "cloud") {
            page.cloudEnabled = true
            page.matterEnabled = false
        } else {
            page.matterEnabled = true
            page.cloudEnabled = false
        }
    }

    Component.onCompleted: {
        backendBox.currentIndex = backendBox.indexOfValue(page.backend)
        runSwitch.checked = page.backend === "cloud" ? page.cloudEnabled : page.matterEnabled
    }

    QQC.ComboBox {
        id: backendBox
        Kirigami.FormData.label: qsTr("Backend to control:")
        textRole: "text"
        valueRole: "value"
        model: [
            { value: "matter", text: qsTr("Matter daemon (local, cool/heat only)") },
            { value: "cloud", text: qsTr("ConnectLife cloud (full control)") }
        ]
        onActivated: {
            page.backend = currentValue
            page.runSelected()
            runSwitch.checked = true
        }
    }

    QQC.Switch {
        id: runSwitch
        Kirigami.FormData.label: qsTr("Run the selected daemon")
        onToggled: {
            if (page.backend === "cloud")
                page.cloudEnabled = checked
            else
                page.matterEnabled = checked
        }
    }

    QQC.Label {
        Kirigami.FormData.isSection: true
        text: qsTr("Only one daemon runs at a time. Selecting a backend starts "
                   + "and enables its systemd user unit (so it also comes back after a "
                   + "reboot) and stops and disables the other. Turning the switch off "
                   + "stops and disables the selected daemon. The cloud daemon reads "
                   + "credentials from ~/.config/hisenseac/connectlife.conf.")
        wrapMode: Text.WordWrap
        opacity: 0.7
    }
}
