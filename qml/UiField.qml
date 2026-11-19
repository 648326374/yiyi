import QtQuick
import QtQuick.Controls

TextField {
    id: control
    implicitHeight: 44
    color: Theme.text
    placeholderTextColor: Theme.muted
    font.family: Theme.font
    font.pixelSize: 13
    leftPadding: 14; rightPadding: 14
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    selectByMouse: true
    background: Rectangle {
        color: Theme.surface
        border.color: control.activeFocus ? Theme.accent : Theme.border
        radius: 8
    }
}
