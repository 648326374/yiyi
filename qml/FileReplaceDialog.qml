import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: dialog
    objectName: "fileReplaceDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(500, parent.width - 40)
    modal: true; padding: 20; closePolicy: Popup.CloseOnEscape
    background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.border }
    onRejected: FileImport.cancelReplacement()
    contentItem: ColumnLayout {
        spacing: 18
        UiText { text: "保存当前文档译文？"; font.pixelSize: 20; font.weight: Font.DemiBold }
        UiText {
            Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
            text: "当前文档有尚未导出的译文或校对草稿。更换为「" + FileImport.pendingFileName + "」前，可先另存译文。"
                + (FileImport.replacementKind === "txt" ? "直接更换会清空当前 TXT 结果。" : "Word / PPT 进度保存在本机，重新打开原文件可恢复。")
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight; spacing: 8
            UiButton { objectName: "fileReplaceCancel"; text: "取消"; compact: true; onClicked: FileImport.cancelReplacement() }
            UiButton { objectName: "fileReplaceDiscard"; text: "直接更换"; kind: "danger"; compact: true; onClicked: FileImport.confirmReplacement() }
            UiButton {
                objectName: "fileReplaceExport"; text: "导出后更换"; kind: "primary"; compact: true
                onClicked: { saveFile.selectedFile = FileImport.replacementOutput; saveFile.open() }
            }
        }
    }
    FileDialog {
        id: saveFile; objectName: "fileReplaceSaveDialog"
        title: "导出当前文档译文"; fileMode: FileDialog.SaveFile
        nameFilters: FileImport.replacementKind === "pptx" ? ["PowerPoint (*.pptx)"] : FileImport.replacementKind === "docx" ? ["Word (*.docx)"] : ["TXT 文本 (*.txt)"]
        defaultSuffix: FileImport.replacementKind
        onAccepted: FileImport.exportAndReplace(selectedFile)
    }
    Connections {
        target: FileImport
        function onPendingChanged() {
            if (FileImport.confirmationPending) dialog.open()
            else dialog.close()
        }
    }
}
