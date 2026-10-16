import QtQuick
import QtQuick.Controls as QQC
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: page

    property alias cfg_command: commandField.text
    property string command: "hisense-ac"

    Component.onCompleted: commandField.text = page.command

    QQC.TextField {
        id: commandField
        Kirigami.FormData.label: "Command:"
        placeholderText: "hisense-ac"
    }
}
