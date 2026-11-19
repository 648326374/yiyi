import QtQuick
import QtQuick.Controls

ComboBox {
    id: control
    property bool quiet: false
    implicitHeight: 40
    implicitWidth: 150
    font.family: Theme.font
    font.pixelSize: 12
    leftPadding: 12
    rightPadding: 32
    hoverEnabled: true
    contentItem: UiText {
        text: control.displayText
        color: control.enabled ? Theme.text : Theme.muted
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.pixelSize: 12
    }
    indicator: Icon { name: "chevronDown"; x: control.width - 27; y: (control.height - height) / 2; width: 16; height: 16 }
    background: Rectangle { color: control.quiet ? "transparent" : Theme.surface; radius: 8; border.width: control.quiet && !control.activeFocus ? 0 : 1; border.color: control.activeFocus ? Theme.accent : Theme.border }
    popup: Popup {
        y: control.height + 5
        width: control.width
        padding: 5
        implicitHeight: Math.min(contentItem.implicitHeight + 10, 300)
        background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: 9 }
        contentItem: ListView {
            id: popupList
            objectName: control.objectName + "PopupList"
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollBehavior { flickable: popupList }
            ScrollBar.vertical: UiScrollBar {}
        }
    }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: control.width - 22
        height: 38
        highlighted: control.highlightedIndex === index
        contentItem: UiText { text: control.textRole.length ? modelData[control.textRole] : modelData; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; color: highlighted ? Theme.accent : Theme.text; font.pixelSize: 12 }
        background: Rectangle { radius: 5; color: highlighted ? Theme.accentSoft : "transparent" }
    }
}
