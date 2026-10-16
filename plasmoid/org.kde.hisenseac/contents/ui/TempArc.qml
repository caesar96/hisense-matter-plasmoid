// SPDX-License-Identifier: MIT
//
// Reusable partial-circular temperature selector.
//   * thick rounded arc (blue progress over a pale inactive track)
//   * draggable thumb
//   * pointer position is mapped to an angle along the arc and clamped to [from, to]
//   * emits `moved(value)` once, on release, so callers don't spam the backend
import QtQuick

Item {
    id: arc

    property real from: 16
    property real to: 32
    property real value: 25
    property real stepSize: 1

    property color activeColor: "#2f7de1"
    property color inactiveColor: "#d5e2f7"
    property color thumbColor: "#ffffff"
    property color thumbBorderColor: "#2f7de1"

    signal moved(real value)

    // Gauge geometry: 270 degrees, open at the bottom.
    readonly property real _startDeg: 135
    readonly property real _sweepDeg: 270
    readonly property real _cx: width / 2
    readonly property real _cy: height / 2
    readonly property real thickness: Math.max(10, Math.round(Math.min(width, height) * 0.085))
    readonly property real thumbSize: Math.max(14, thickness * 1.25)
    readonly property real radius: Math.min(width, height) / 2 - thickness / 2 - thumbSize / 2 - 1

    property bool dragging: false
    property real dragValue: 25

    // While the user is dragging show that value, otherwise the bound value.
    readonly property real displayValue: dragging ? dragValue : value

    function _snap(v) {
        v = Math.max(from, Math.min(to, v))
        if (stepSize > 0)
            v = from + Math.round((v - from) / stepSize) * stepSize
        return Math.round(v * 100) / 100
    }

    function _valueToRad(v) {
        const t = (to === from) ? 0 : (v - from) / (to - from)
        return (_startDeg + t * _sweepDeg) * Math.PI / 180
    }

    // Map an angle (degrees, canvas convention: +x = 0, clockwise with y-down)
    // to a temperature. The bottom gap is clamped to the nearest end.
    function _degToValue(deg) {
        let d = ((deg % 360) + 360) % 360
        const gapEnd = _startDeg + _sweepDeg - 360 // 45
        let rel
        if (d >= _startDeg)
            rel = d - _startDeg
        else if (d <= gapEnd)
            rel = d + (360 - _startDeg)
        else
            rel = (d < (_startDeg + gapEnd) / 2) ? _sweepDeg : 0
        const t = Math.max(0, Math.min(1, rel / _sweepDeg))
        return _snap(from + t * (to - from))
    }

    // Returns a temperature for a point, or null if the point is too close to
    // the centre to be a meaningful angle.
    function valueAt(px, py) {
        const dx = px - _cx
        const dy = py - _cy
        if (Math.sqrt(dx * dx + dy * dy) < radius * 0.3)
            return null
        return _degToValue(Math.atan2(dy, dx) * 180 / Math.PI)
    }

    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onDisplayValueChanged: canvas.requestPaint()
    onActiveColorChanged: canvas.requestPaint()
    onInactiveColorChanged: canvas.requestPaint()
    onThumbColorChanged: canvas.requestPaint()
    onThumbBorderColorChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            const cx = arc._cx
            const cy = arc._cy
            const r = arc.radius
            const startRad = arc._startDeg * Math.PI / 180
            const endRad = (arc._startDeg + arc._sweepDeg) * Math.PI / 180
            const valueRad = arc._valueToRad(arc.displayValue)

            ctx.lineCap = "round"

            // inactive track
            ctx.beginPath()
            ctx.lineWidth = arc.thickness
            ctx.strokeStyle = arc.inactiveColor
            ctx.arc(cx, cy, r, startRad, endRad, false)
            ctx.stroke()

            // active progress
            ctx.beginPath()
            ctx.lineWidth = arc.thickness
            ctx.strokeStyle = arc.activeColor
            ctx.arc(cx, cy, r, startRad, valueRad, false)
            ctx.stroke()

            // thumb
            const tx = cx + r * Math.cos(valueRad)
            const ty = cy + r * Math.sin(valueRad)
            ctx.beginPath()
            ctx.fillStyle = arc.thumbColor
            ctx.arc(tx, ty, arc.thumbSize / 2, 0, 2 * Math.PI)
            ctx.fill()
            ctx.lineWidth = 3
            ctx.strokeStyle = arc.thumbBorderColor
            ctx.stroke()
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: arc.enabled
        preventStealing: true

        onPressed: (m) => {
            const v = arc.valueAt(m.x, m.y)
            if (v === null)
                return
            arc.dragging = true
            arc.dragValue = v
        }
        onPositionChanged: (m) => {
            if (!arc.dragging)
                return
            const v = arc.valueAt(m.x, m.y)
            if (v !== null)
                arc.dragValue = v
        }
        onReleased: (m) => {
            if (!arc.dragging)
                return
            arc.dragging = false
            arc.moved(arc.dragValue)
        }
        onCanceled: arc.dragging = false
    }
}
