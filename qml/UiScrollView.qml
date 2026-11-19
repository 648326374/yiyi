import QtQuick
import QtQuick.Controls

ScrollView {
    id: view
    clip: true
    contentWidth: availableWidth
    // Reserve a constant gutter so the thumb never changes text wrapping.
    rightPadding: 12
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical: UiScrollBar {
        objectName: view.objectName + "Bar"
        parent: view
        x: view.width - width
        y: view.topPadding
        height: view.availableHeight
    }
    ScrollBehavior { flickable: view.contentItem }
}
