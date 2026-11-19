import QtQuick
import QtQuick.Window

Item {
    id: handles
    required property var targetWindow
    visible: targetWindow.visibility !== Window.Maximized && targetWindow.visibility !== Window.FullScreen
    Repeater {
        model: [
            {name: "left", edge: Qt.LeftEdge, cursor: Qt.SizeHorCursor, horizontal: false, right: false, bottom: false, corner: false},
            {name: "right", edge: Qt.RightEdge, cursor: Qt.SizeHorCursor, horizontal: false, right: true, bottom: false, corner: false},
            {name: "top", edge: Qt.TopEdge, cursor: Qt.SizeVerCursor, horizontal: true, right: false, bottom: false, corner: false},
            {name: "bottom", edge: Qt.BottomEdge, cursor: Qt.SizeVerCursor, horizontal: true, right: false, bottom: true, corner: false},
            {name: "top-left", edge: Qt.TopEdge | Qt.LeftEdge, cursor: Qt.SizeFDiagCursor, horizontal: false, right: false, bottom: false, corner: true},
            {name: "top-right", edge: Qt.TopEdge | Qt.RightEdge, cursor: Qt.SizeBDiagCursor, horizontal: false, right: true, bottom: false, corner: true},
            {name: "bottom-left", edge: Qt.BottomEdge | Qt.LeftEdge, cursor: Qt.SizeBDiagCursor, horizontal: false, right: false, bottom: true, corner: true},
            {name: "bottom-right", edge: Qt.BottomEdge | Qt.RightEdge, cursor: Qt.SizeFDiagCursor, horizontal: false, right: true, bottom: true, corner: true}
        ]
        delegate: MouseArea {
            required property var modelData
            objectName: "resize-" + modelData.name
            width: modelData.corner ? 10 : modelData.horizontal ? handles.width - 20 : 5
            height: modelData.corner ? 10 : modelData.horizontal ? 5 : handles.height - 20
            x: modelData.right ? handles.width - width : modelData.corner || !modelData.horizontal ? 0 : 10
            y: modelData.bottom ? handles.height - height : modelData.corner || modelData.horizontal ? 0 : 10
            cursorShape: modelData.cursor
            acceptedButtons: Qt.LeftButton
            onPressed: function(mouse) { mouse.accepted = handles.targetWindow.startSystemResize(modelData.edge) }
        }
    }
}
