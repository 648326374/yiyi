import QtQuick
import QtQuick.Controls
import QtQuick.Window

ToolTip {
    id: tip
    required property Item targetControl
    property bool below: false
    property bool dismissed: false
    readonly property var hostWindow: targetControl.Window.window
    objectName: targetControl.objectName + "ToolTip"
    parent: targetControl
    popupType: Popup.Item
    x: (targetControl.width - implicitWidth) / 2
    y: below ? targetControl.height + 8 : -implicitHeight - 8
    margins: 8
    leftPadding: 11; rightPadding: 11
    topPadding: 7; bottomPadding: 7
    delay: 550
    timeout: 3000
    visible: targetControl.enabled && targetControl.hovered && !targetControl.down
        && !dismissed && text.length > 0 && !!hostWindow && hostWindow.active
        && hostWindow.visible && hostWindow.visibility !== Window.Minimized
    contentItem: UiText {
        text: tip.text
        font.pixelSize: 11
        color: "#f1f7f2"
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
    }
    background: Rectangle {
        radius: 6
        color: Theme.dark ? "#304638" : "#293f32"
        border.color: Theme.dark ? "#405747" : "#293f32"
    }
    Connections {
        target: tip.targetControl
        function onPressed() { tip.dismissed = true }
        function onHoveredChanged() { tip.dismissed = false }
    }
    Connections {
        target: tip.hostWindow
        function onActiveChanged() {
            if (!tip.hostWindow || !tip.hostWindow.active) tip.dismissed = true
        }
        function onVisibilityChanged() { tip.dismissed = true }
    }
}
