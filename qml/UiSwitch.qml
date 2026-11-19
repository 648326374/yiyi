import QtQuick
import QtQuick.Controls

Switch {
    id: control
    implicitWidth: 42; implicitHeight: 26
    padding: 0; hoverEnabled: true
    indicator: Rectangle {
        implicitWidth: 42; implicitHeight: 24
        y: (control.height - height) / 2
        radius: 12
        color: control.checked ? Theme.accent : Theme.border
        opacity: control.enabled ? 1 : 0.5
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.text
        Rectangle {
            x: control.checked ? 21 : 3; y: 3
            width: 18; height: 18; radius: 9
            color: control.checked ? Theme.accentText : Theme.surface
            Behavior on x { NumberAnimation { duration: 100 } }
        }
    }
    contentItem: Item {}
    background: Item {}
}
