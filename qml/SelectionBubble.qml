import QtQuick
import QtQuick.Controls
import QtQuick.Window

Window {
    id: bubble
    objectName: "selectionBubble"
    width: 112; height: 42
    x: Desktop.bubbleX; y: Desktop.bubbleY
    visible: Desktop.selectionVisible
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    transientParent: null
    color: "transparent"
    title: "译意划词"
    Button {
        id: query
        objectName: "selectionQueryButton"
        anchors.fill: parent; anchors.margins: 2
        hoverEnabled: true; focusPolicy: Qt.NoFocus
        Accessible.name: "查询选中文字"
        onClicked: Desktop.acceptSelection()
        background: Rectangle {
            radius: 10
            color: query.down ? Theme.accentStrong : query.hovered ? Theme.accentSoftHover : Theme.surface
            border.color: Theme.accent
        }
        contentItem: Row {
            spacing: 9
            leftPadding: 10
            Icon { name: "translate"; width: 20; height: 20; anchors.verticalCenter: parent.verticalCenter; color: query.down ? Theme.accentText : Theme.accent }
            UiText { text: "查词"; font.pixelSize: 13; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter; color: query.down ? Theme.accentText : Theme.text }
        }
    }
}
