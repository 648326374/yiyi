import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Rectangle {
    id: card
    implicitHeight: desktopColumn.implicitHeight + 36
    radius: 14; color: Theme.surface; border.color: Theme.border
    function reloadShortcuts() {
        shortcutsSwitch.checked = Desktop.shortcutsEnabled
        showField.shortcut = Desktop.showShortcut
        selectionField.shortcut = Desktop.selectionShortcut
    }
    ColumnLayout {
        id: desktopColumn
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 18 }
        spacing: 16
        RowLayout {
            spacing: 10
            Icon { name: "keyboard"; color: Theme.accent; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
            UiText { text: "桌面与划词"; font.pixelSize: 15; font.weight: Font.DemiBold }
            Item { Layout.fillWidth: true }
            UiText { text: Desktop.trayAvailable ? "托盘已就绪" : "托盘未启用"; color: Theme.muted; font.pixelSize: 11 }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 18
            ColumnLayout {
                Layout.fillWidth: true; spacing: 5
                UiText { text: "关闭窗口后留在托盘"; font.pixelSize: 13 }
                UiText { text: "在后台继续使用快捷键和划词。右键托盘图标可退出译意。"; color: Theme.secondary; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            UiSwitch {
                objectName: "closeToTraySwitch"
                checked: Desktop.closeToTray
                Accessible.name: "关闭窗口后留在托盘"
                onToggled: Desktop.closeToTray = checked
            }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 18
            ColumnLayout {
                Layout.fillWidth: true; spacing: 5
                UiText { text: "划选文字后显示浮标"; font.pixelSize: 13 }
                UiText { text: "拖动划选或双击选词，点击浮标才查询。可在托盘菜单随时暂停。"; color: Theme.secondary; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            UiSwitch {
                objectName: "selectionEnabledSwitch"
                checked: Desktop.selectionEnabled
                Accessible.name: "划选文字后显示浮标"
                onToggled: Desktop.selectionEnabled = checked
            }
        }
        UiText { visible: Desktop.selectionError.length > 0; text: Desktop.selectionError; color: Theme.danger; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        RowLayout {
            Layout.fillWidth: true
            UiText { text: "全局快捷键"; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            UiSwitch { id: shortcutsSwitch; objectName: "shortcutsEnabledSwitch"; Accessible.name: "全局快捷键" }
        }
        GridLayout {
            Layout.fillWidth: true; columns: 2
            columnSpacing: 20; rowSpacing: 10
            enabled: shortcutsSwitch.checked
            UiText { text: "打开翻译工作台"; color: Theme.secondary; font.pixelSize: 12; Layout.fillWidth: true }
            ShortcutField { id: showField; objectName: "showShortcutField"; Layout.preferredWidth: 190 }
            UiText { text: "查询选中文字"; color: Theme.secondary; font.pixelSize: 12; Layout.fillWidth: true }
            ShortcutField { id: selectionField; objectName: "selectionShortcutField"; Layout.preferredWidth: 190 }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 12
            UiText { text: "点击快捷键框后按下新的组合键，再保存。"; color: Theme.muted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
            UiButton {
                text: "恢复默认"; compact: true; kind: "ghost"
                onClicked: { showField.shortcut = "Ctrl+Alt+Y"; selectionField.shortcut = "Ctrl+Alt+D"; shortcutsSwitch.checked = true }
            }
            UiButton {
                objectName: "saveShortcutsButton"
                text: "保存快捷键"; compact: true; kind: "soft"
                onClicked: if (Desktop.applyShortcuts(shortcutsSwitch.checked, showField.shortcut, selectionField.shortcut)) Desktop.toast("快捷键已保存")
            }
        }
        UiText { objectName: "shortcutErrorLabel"; visible: Desktop.shortcutError.length > 0; text: Desktop.shortcutError; color: Theme.danger; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
        UiText {
            text: "浮标取词不修改剪贴板。部分应用不提供可选文字，或需要相同的运行权限；此时可尝试取词快捷键，或复制后粘贴。快捷键取词会在必要时执行复制，并尽量还原原剪贴板。"
            color: Theme.muted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap; lineHeight: 1.5
        }
    }
    Binding {
        target: Desktop; property: "recordingShortcut"
        value: !!card.Window.window && card.Window.window.visible && card.Window.window.active
            && (showField.activeFocus || selectionField.activeFocus)
    }
    Connections {
        target: Desktop
        function onPreferencesChanged() { card.reloadShortcuts() }
        function onShortcutRecorded(sequence) {
            if (showField.activeFocus) { showField.shortcut = sequence; showField.focus = false }
            else if (selectionField.activeFocus) { selectionField.shortcut = sequence; selectionField.focus = false }
        }
    }
    Component.onCompleted: reloadShortcuts()
}
