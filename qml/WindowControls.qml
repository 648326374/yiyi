import QtQuick
import QtQuick.Controls
import QtQuick.Window

Row {
    id: controls
    required property var targetWindow
    spacing: 0
    Repeater {
        model: [{action: "minimize", label: "最小化"}, {action: "maximize", label: "最大化"}, {action: "close", label: "关闭"}]
        delegate: Button {
            id: button
            required property var modelData
            objectName: "window-" + modelData.action
            width: 46; height: 36
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            readonly property bool pointerOver: hovered && controls.targetWindow.active
            Accessible.name: modelData.action === "maximize" && controls.targetWindow.visibility === Window.Maximized ? "还原窗口" : modelData.label
            UiToolTip { targetControl: button; text: button.Accessible.name; below: true }
            contentItem: Item {
                Icon {
                    anchors.centerIn: parent
                    width: 15; height: 15
                    name: button.modelData.action === "maximize" && controls.targetWindow.visibility === Window.Maximized ? "restore" : button.modelData.action
                    color: button.modelData.action === "close" && (button.pointerOver || button.down) ? "#ffffff" : Theme.secondary
                }
            }
            background: Rectangle {
                color: button.modelData.action === "close" ? (button.down ? "#a83d38" : button.pointerOver ? "#c74c47" : "transparent")
                    : button.down ? Theme.border : button.pointerOver ? Theme.hover : "transparent"
                border.width: button.visualFocus ? 1 : 0
                border.color: Theme.accent
            }
            onClicked: {
                if (modelData.action === "minimize") controls.targetWindow.showMinimized()
                else if (modelData.action === "maximize") controls.targetWindow.toggleMaximized()
                else controls.targetWindow.close()
            }
        }
    }
}
