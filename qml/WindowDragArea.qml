import QtQuick

MouseArea {
    id: area
    required property var targetWindow
    property point pressPosition: Qt.point(0, 0)
    property bool moveStarted: false
    acceptedButtons: Qt.LeftButton
    preventStealing: true
    onPressed: function(mouse) {
        pressPosition = Qt.point(mouse.x, mouse.y)
        moveStarted = false
    }
    onPositionChanged: function(mouse) {
        if (!pressed || moveStarted) return
        const distance = Math.abs(mouse.x - pressPosition.x) + Math.abs(mouse.y - pressPosition.y)
        if (distance >= Qt.styleHints.startDragDistance) {
            moveStarted = true
            targetWindow.startSystemMove()
        }
    }
    onReleased: moveStarted = false
    onCanceled: moveStarted = false
    onDoubleClicked: targetWindow.toggleMaximized()
}
