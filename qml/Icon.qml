import QtQuick
import QtQuick.Shapes
import "IconPaths.js" as Icons

Item {
    id: root
    property string name: "sparkles"
    property color color: Theme.secondary
    property bool filled: false
    implicitWidth: 20
    implicitHeight: 20
    // The software Shape fallback needs a layer for viewport clipping. Cache
    // at the final icon size, after the 24-unit vector transform; GPU backends
    // render the vector directly to retain crisp strokes at every scale.
    layer.enabled: GraphicsInfo.api === GraphicsInfo.Software
    layer.smooth: true
    Item {
        width: 24; height: 24
        anchors.centerIn: parent
        scale: root.width / 24
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: root.color
                strokeWidth: 1.65
                fillColor: root.filled ? root.color : "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Icons.paths[root.name] || Icons.paths["sparkles"] }
            }
        }
    }
}
