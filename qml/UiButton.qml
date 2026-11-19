import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: control
    property string kind: "outline"
    property string iconName: ""
    property string hint: ""
    property bool compact: false
    readonly property color foreground: !enabled ? Theme.muted : kind === "primary" ? Theme.accentText : kind === "danger" ? Theme.danger : (kind === "soft" ? Theme.accent : Theme.secondary)
    implicitHeight: compact ? 34 : 42
    implicitWidth: Math.max(compact ? 34 : 42, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: compact ? 9 : 16
    rightPadding: leftPadding
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: text.length ? text : hint
    UiToolTip { targetControl: control; text: control.hint }
    contentItem: Item {
        implicitWidth: buttonRow.implicitWidth
        implicitHeight: buttonRow.implicitHeight
        Row {
            id: buttonRow
            anchors.centerIn: parent
            spacing: 8
            Icon { visible: control.iconName.length > 0; name: control.iconName; color: control.foreground; width: 17; height: 17; anchors.verticalCenter: parent.verticalCenter }
            UiText { visible: text.length > 0; text: control.text; color: control.foreground; font.pixelSize: 12; font.weight: control.kind === "primary" ? Font.DemiBold : Font.Normal; anchors.verticalCenter: parent.verticalCenter }
        }
    }
    background: Rectangle {
        radius: 9
        color: control.kind === "primary" ? Theme.accent : control.kind === "soft" ? Theme.accentSoft
            : control.kind === "ghost" ? "transparent" : Theme.surface
        opacity: control.enabled ? 1 : 0.55
        border.width: control.visualFocus ? 2 : control.kind === "outline" ? 1 : 0
        border.color: control.visualFocus ? Theme.accent : Theme.border
        Rectangle {
            anchors.fill: parent
            anchors.margins: parent.border.width
            radius: Math.max(0, parent.radius - parent.border.width)
            color: control.kind === "primary" ? Theme.accentStrong
                : control.kind === "soft" ? (control.down ? Theme.accentSoftPressed : Theme.accentSoftHover)
                : control.down ? Theme.border : Theme.hover
            opacity: control.enabled && (control.hovered || control.down) ? 1 : 0
            // Keep RGB fixed and animate coverage. Interpolating a light fill
            // to transparent black produces a dark flash in both directions.
            Behavior on opacity { NumberAnimation { duration: 90 } }
        }
    }
}
