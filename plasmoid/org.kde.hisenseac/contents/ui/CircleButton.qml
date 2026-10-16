// SPDX-License-Identifier: MIT
//
// Small round icon button used for power, +/-, and the quick-action row.
// Wraps QQC.AbstractButton so we keep focus/hover/press handling for free.
import QtQuick
import QtQuick.Controls as QQC

import org.kde.kirigami as Kirigami

QQC.AbstractButton {
    id: control

    property string iconName: ""
    property int diameter: Kirigami.Units.gridUnit * 2.75
    property color bgColor: Kirigami.Theme.backgroundColor
    property color accentColor: Kirigami.Theme.highlightColor
    property bool emphasized: false

    readonly property bool _isDark: Kirigami.Theme.backgroundColor.hslLightness < 0.5

    implicitWidth: diameter
    implicitHeight: diameter

    background: Rectangle {
        radius: width / 2
        color: {
            if (!control.enabled)
                return Qt.rgba(Kirigami.Theme.textColor.r,
                               Kirigami.Theme.textColor.g,
                               Kirigami.Theme.textColor.b, 0.06)
            if (control.pressed)
                return Qt.rgba(control.accentColor.r, control.accentColor.g,
                               control.accentColor.b, 0.24)
            if (control.hovered)
                return Qt.rgba(control.accentColor.r, control.accentColor.g,
                               control.accentColor.b, 0.14)
            return control.emphasized ? control.accentColor : control.bgColor
        }
        border.width: control.emphasized ? 0 : 1
        border.color: Qt.rgba(Kirigami.Theme.textColor.r,
                              Kirigami.Theme.textColor.g,
                              Kirigami.Theme.textColor.b, control._isDark ? 0.18 : 0.10)
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Kirigami.Icon {
        source: control.iconName
        color: {
            if (!control.enabled)
                return Qt.rgba(Kirigami.Theme.textColor.r,
                               Kirigami.Theme.textColor.g,
                               Kirigami.Theme.textColor.b, 0.35)
            if (control.emphasized)
                return Kirigami.Theme.highlightedTextColor
            return Kirigami.Theme.textColor
        }
        implicitWidth: Math.round(control.diameter * 0.42)
        implicitHeight: implicitWidth
    }
}
