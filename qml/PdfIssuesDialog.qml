import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog; objectName: "pdfIssuesDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(640, parent.width - 40); height: Math.min(560, parent.height - 40)
    modal: true; padding: 20
    property var entries: []
    signal locateBlock(string blockId)
    onOpened: entries = Pdf.issuePages()
    background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 12
        UiText { text: "文档进度与校对"; font.pixelSize: 18; font.weight: Font.DemiBold }
        UiText {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary; font.pixelSize: 12
            text: "已解析 " + Pdf.analyzedPages + " / " + Pdf.pageCount + " 页 · 已翻译 " + Pdf.completed + " / " + Pdf.total + " 段\n"
                + Pdf.pendingPages + " 页待处理 · " + Pdf.reviewPages + " 页待校对" + (Pdf.issueIndexing ? " · 正在整理校对项…" : "")
        }
        ListView {
            id: pages; objectName: "pdfIssuePages"; Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredHeight: 190
            clip: true; model: dialog.entries; spacing: 4; boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: UiScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                objectName: "pdfIssuePage_" + modelData.page; width: pages.width; height: Math.max(48, reason.implicitHeight + 20)
                onClicked: Pdf.page = modelData.page
                background: Rectangle { radius: 7; color: Pdf.page === modelData.page ? Theme.accentSoft : parent.hovered ? Theme.hover : Theme.subtle }
                contentItem: RowLayout {
                    spacing: 12
                    UiText { text: "第 " + (modelData.page + 1) + " 页"; font.pixelSize: 12 }
                    UiText { id: reason; Layout.fillWidth: true; text: modelData.reason; textFormat: Text.PlainText; wrapMode: Text.Wrap; font.pixelSize: 11; color: modelData.severity === "error" ? Theme.danger : modelData.severity === "review" ? Theme.warning : Theme.secondary }
                    UiText { text: "›"; color: Theme.secondary }
                }
            }
            UiText { anchors.centerIn: parent; visible: pages.count === 0; text: "所有页面已处理"; color: Theme.secondary }
        }
        UiText { text: "第 " + (Pdf.page + 1) + " 页 · " + (Pdf.pageIssues.length ? "点击问题定位到段落" : Pdf.pageStatus); font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
        UiText { objectName: "pdfPageDetail"; text: Pdf.warning || Pdf.pageNotice; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight; font.pixelSize: 11; color: Theme.secondary }
        ListView {
            id: blocks; objectName: "pdfIssueBlocks"; Layout.fillWidth: true; Layout.preferredHeight: 100; visible: count > 0
            clip: true; model: Pdf.pageIssues; spacing: 4; boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: UiScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                objectName: "pdfIssueBlock_" + modelData.blockId; width: blocks.width; height: 54
                enabled: !Pdf.busy && !Pdf.processing
                onClicked: { dialog.close(); dialog.locateBlock(modelData.blockId) }
                background: Rectangle { radius: 7; color: parent.hovered ? Theme.hover : Theme.subtle }
                contentItem: ColumnLayout {
                    spacing: 4
                    UiText { text: modelData.reason; Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.warning; font.pixelSize: 11; textFormat: Text.PlainText }
                    UiText { text: modelData.source.replace(/\n/g, " "); Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.secondary; font.pixelSize: 11; textFormat: Text.PlainText }
                }
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            UiButton { objectName: "pdfIssuesClose"; text: "返回阅读"; compact: true; kind: "primary"; onClicked: dialog.close() }
        }
    }
    Connections { target: Pdf; function onChanged() { if (dialog.opened) dialog.entries = Pdf.issuePages() } }
}
