import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

DropArea {
    id: area
    objectName: "fileDropArea"
    property bool blocked: false
    property var fileUrls: []
    readonly property var fileInfo: fileUrls.length ? FileImport.inspect(fileUrls) : ({})
    readonly property string reason: blocked ? "请先关闭当前对话框，再拖入文件。"
        : fileInfo.error || (fileInfo.kind === "txt" ? FileImport.textBlockReason
        : fileInfo.kind === "pdf" ? FileImport.pdfBlockReason : fileInfo.kind === "docx" || fileInfo.kind === "pptx" ? FileImport.officeBlockReason : "")
    signal rejected(string message)

    onEntered: function(drag) {
        // Handle file URLs above editors, while leaving ordinary text drags alone.
        if (!drag.hasUrls) { drag.accepted = false; return }
        fileUrls = drag.urls
        drag.accepted = true
    }
    onExited: fileUrls = []
    onDropped: function(drop) {
        if (!drop.hasUrls) { drop.accepted = false; return }
        if (blocked) area.rejected(reason)
        else FileImport.requestDrop(drop.urls)
        // Consume unsupported/busy file drops too, so paths never enter editors.
        drop.accept(Qt.CopyAction)
        fileUrls = []
    }

    Item {
        objectName: "fileDropOverlay"
        anchors.fill: parent
        visible: area.containsDrag
        Rectangle { anchors.fill: parent; color: Theme.background; opacity: 0.94 }
        Rectangle {
            anchors.fill: parent; anchors.margins: 10
            radius: 14; color: "transparent"
            border.width: 2; border.color: area.reason.length ? Theme.danger : Theme.accent
        }
        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(460, parent.width - 56)
            spacing: 16
            Icon {
                name: "document"; color: area.reason.length ? Theme.danger : Theme.accent
                Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 40; Layout.preferredHeight: 40
            }
            UiText {
                objectName: "fileDropTitle"
                Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter
                text: area.reason.length ? "暂时无法导入" : "松开以打开 " + (area.fileInfo.kind || "文件").toUpperCase()
                font.pixelSize: 24; font.weight: Font.DemiBold
            }
            UiText {
                objectName: "fileDropDetail"
                Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                text: area.reason || area.fileInfo.fileName || ""
                textFormat: Text.PlainText; color: area.reason.length ? Theme.danger : Theme.secondary
            }
            UiText {
                Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; color: Theme.muted; font.pixelSize: 12
                text: "一次导入一个 TXT、DOCX、PPTX 或 PDF，打开后手动开始翻译。"
            }
        }
    }
}
