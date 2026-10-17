import QtQuick
import QtQuick.Controls as QQC
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: page

    // Values: "auto" | "matter" | "cloud".
    property string backend: "auto"
    property alias cfg_backend: page.backend

    Component.onCompleted: backendBox.currentIndex = backendBox.indexOfValue(page.backend)

    QQC.ComboBox {
        id: backendBox
        Kirigami.FormData.label: qsTr("Daemon:")
        textRole: "text"
        valueRole: "value"
        model: [
            { value: "auto", text: qsTr("Automatic (prefer ConnectLife cloud)") },
            { value: "matter", text: qsTr("Matter daemon (local, cool/heat only)") },
            { value: "cloud", text: qsTr("ConnectLife cloud (full control)") }
        ]
        onActivated: page.backend = currentValue
    }

    QQC.Label {
        Kirigami.FormData.isSection: true
        text: qsTr("The ConnectLife cloud daemon (hisense-connectlife) reads its "
                   + "credentials from ~/.config/hisenseac/connectlife.conf and "
                   + "exposes every mode, fan speed, swing and eco setting. The "
                   + "Matter daemon (hisense-matterd) works fully offline but only "
                   + "exposes cooling and heating.")
        wrapMode: Text.WordWrap
        opacity: 0.7
    }
}
