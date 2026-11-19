import QtQuick
import QtQuick.Controls

UiField {
    id: control
    property string shortcut: ""
    readOnly: true
    selectByMouse: false
    text: activeFocus ? "请按下组合键…" : shortcut
    horizontalAlignment: Text.AlignHCenter
    font.family: "Segoe UI"
    font.pixelSize: 12
    Accessible.name: "快捷键，点击后按下组合键"
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) { event.accepted = false; return }
        event.accepted = true
        if (event.key === Qt.Key_Escape) { focus = false; return }
        var value = Desktop.shortcutFromKey(event.key, event.modifiers)
        if (value.length > 0) { shortcut = value; focus = false }
    }
}
