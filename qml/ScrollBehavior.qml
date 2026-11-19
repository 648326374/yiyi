import QtQuick

WheelHandler {
    id: behavior
    required property var flickable
    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    property Binding bounds: Binding {
        target: behavior.flickable
        property: "boundsBehavior"
        value: Flickable.StopAtBounds
        when: !!behavior.flickable
    }
    property Binding movement: Binding {
        target: behavior.flickable
        property: "boundsMovement"
        value: Flickable.StopAtBounds
        when: !!behavior.flickable
    }
    property Binding direction: Binding {
        target: behavior.flickable
        property: "flickableDirection"
        value: Flickable.VerticalFlick
        when: !!behavior.flickable
    }
    onWheel: function(event) {
        const view = behavior.flickable
        if (!view) { event.accepted = false; return }
        const distance = event.pixelDelta.y !== 0 ? event.pixelDelta.y
            : event.angleDelta.y / 120 * Math.max(1, Qt.styleHints.wheelScrollLines) * 20
        if (distance === 0) { event.accepted = false; return }
        const first = view.originY - view.topMargin
        const last = Math.max(first, view.originY + view.contentHeight - view.height + view.bottomMargin)
        // A new wheel direction takes effect immediately, including at an edge.
        // Precision touchpads already provide their own momentum as pixel deltas.
        view.cancelFlick()
        view.contentY = Math.max(first, Math.min(last, view.contentY - distance))
        event.accepted = true
    }
}
