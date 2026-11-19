import QtQuick
import QtQuick.Controls

ScrollBar {
    id: bar
    implicitWidth: 12
    implicitHeight: 12
    padding: 3
    minimumSize: 0.055
    hoverEnabled: true
    interactive: true
    policy: ScrollBar.AsNeeded
    visible: policy === ScrollBar.AlwaysOn || (policy === ScrollBar.AsNeeded && size < 1)
    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 28
        radius: 3
        color: bar.pressed ? Theme.accent : Theme.secondary
        opacity: bar.pressed || bar.hovered ? 0.85 : bar.active ? 0.55 : 0.3
        Behavior on opacity { NumberAnimation { duration: 100 } }
    }
    background: Rectangle {
        radius: 5
        color: bar.hovered || bar.pressed ? Theme.subtle : "transparent"
    }
}
