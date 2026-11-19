import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ColumnLayout {
    id: page
    signal openSettings()
    readonly property var languages: ["简体中文", "繁體中文", "英语", "日语", "韩语", "法语", "德语", "西班牙语", "俄语", "葡萄牙语", "意大利语", "阿拉伯语"]
    readonly property var encodings: ["auto", "utf8", "gb18030", "utf16le", "utf16be"]
    readonly property bool fileDialogOpen: openFile.visible || saveFile.visible
    spacing: 12

    FileDialog {
        id: openFile; objectName: "documentOpenDialog"
        title: "导入 TXT 文档"
        nameFilters: ["TXT 文本 (*.txt)"]
        onAccepted: FileImport.requestOpen(selectedFile)
    }
    FileDialog {
        id: saveFile
        title: Documents.state === "completed" ? "导出完整译文" : "导出已完成部分"
        fileMode: FileDialog.SaveFile
        nameFilters: ["TXT 文本 (*.txt)"]
        defaultSuffix: "txt"
        onAccepted: Documents.saveFile(selectedFile)
    }
    RowLayout {
        Layout.fillWidth: true
        ColumnLayout {
            spacing: 5
            UiText { text: "文档翻译"; font.pixelSize: 22; font.weight: Font.DemiBold }
            UiText { text: "按段翻译，慢慢读懂整篇文字。"; color: Theme.secondary; font.pixelSize: 12 }
        }
        Item { Layout.fillWidth: true }
        UiButton { objectName: "documentImportButton"; text: Documents.sourceText.length ? "更换 TXT" : "导入 TXT"; iconName: "document"; kind: "soft"; enabled: !Documents.busy; onClicked: openFile.open() }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 8
        UiText { text: "编码"; font.pixelSize: 11; color: Theme.secondary }
        UiCombo {
            id: encodingSelector
            objectName: "documentEncoding"
            model: ["自动 / UTF-8", "UTF-8", "GB18030 / GBK", "UTF-16 LE", "UTF-16 BE"]
            currentIndex: page.encodings.indexOf(FileImport.textEncoding)
            onActivated: FileImport.textEncoding = page.encodings[currentIndex]
            Layout.preferredWidth: 148; enabled: !Documents.busy
        }
        Item { Layout.fillWidth: true }
        UiCombo {
            objectName: "documentProvider"
            Layout.preferredWidth: 170
            model: App.providers; textRole: "name"
            displayText: App.providers.length ? App.providerName : "选择 AI 服务"
            currentIndex: {
                for (var i = 0; i < App.providers.length; ++i) if (App.providers[i].id === App.selectedProviderId) return i
                return -1
            }
            enabled: App.providers.length > 0 && !Documents.busy
            onActivated: App.selectProvider(App.providers[currentIndex].id)
        }
        UiButton { iconName: "settings"; compact: true; hint: "管理 AI 服务"; onClicked: page.openSettings() }
    }
    Rectangle {
        visible: Documents.error.length > 0
        Layout.fillWidth: true; implicitHeight: documentError.implicitHeight + 20
        radius: 9; color: Theme.dangerSoft
        UiText {
            id: documentError; objectName: "documentError"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
            text: Documents.error; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: 12; wrapMode: Text.Wrap
        }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 8
        UiCombo { model: ["自动检测"].concat(page.languages); currentIndex: model.indexOf(App.sourceLanguage); onActivated: App.sourceLanguage = currentText; Layout.preferredWidth: 120; enabled: !Documents.busy }
        Icon { name: "arrowRight"; color: Theme.muted; Layout.preferredWidth: 16; Layout.preferredHeight: 16 }
        UiCombo { model: page.languages; currentIndex: model.indexOf(App.targetLanguage); onActivated: App.targetLanguage = currentText; Layout.preferredWidth: 112; enabled: !Documents.busy }
        UiCombo { model: ["自然", "正式", "口语", "学术"]; currentIndex: model.indexOf(App.tone); onActivated: App.tone = currentText; Layout.preferredWidth: 92; enabled: !Documents.busy }
        Item { Layout.fillWidth: true }
        UiText { text: Documents.total ? Documents.sourceText.length.toLocaleString() + " 字符 · " + Documents.total + " 段" : "TXT · 最多 20 万字符"; font.pixelSize: 10; color: Theme.muted }
    }
    RowLayout {
        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14
        Repeater {
            model: [false, true]
            delegate: Rectangle {
                required property bool modelData
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                color: Theme.surface; radius: 12; border.color: Theme.border
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    UiText {
                        Layout.fillWidth: true; Layout.margins: 14
                        text: modelData ? "译文" : Documents.fileName.length ? Documents.fileName + " · " + Documents.encoding : "原文预览"
                        textFormat: Text.PlainText; elide: Text.ElideMiddle; font.pixelSize: 11; color: Theme.secondary
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                    UiScrollView {
                        Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 16
                        clip: true; ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        TextArea {
                            objectName: modelData ? "documentResult" : "documentSource"
                            text: modelData ? Documents.resultText + Documents.preview : Documents.sourceText
                            placeholderText: modelData ? "译文将在这里按段显示。\n停止后可继续，也可导出已完成部分。" : "打开或拖入 TXT，预览原文。\n\n使用当前 AI 服务逐段翻译，\n每段最多约 1,000 字符。"
                            placeholderTextColor: Theme.muted; color: Theme.text
                            textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap; readOnly: true; selectByMouse: true
                            font.family: Theme.font; font.pixelSize: 15
                            selectionColor: Theme.accent; selectedTextColor: Theme.accentText
                            padding: 0; background: null
                        }
                    }
                }
            }
        }
    }
    ColumnLayout {
        Layout.fillWidth: true; spacing: 8
        ProgressBar {
            objectName: "documentProgress"; Layout.fillWidth: true
            from: 0; to: Math.max(1, Documents.total); value: Documents.completed
            background: Rectangle { implicitHeight: 4; radius: 2; color: Theme.border }
            contentItem: Item { implicitHeight: 4; Rectangle { width: parent.width * parent.parent.visualPosition; height: parent.height; radius: 2; color: Theme.accent } }
        }
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true; spacing: 3
                UiText {
                    objectName: "documentStatus"; Layout.fillWidth: true; font.pixelSize: 11; color: Theme.secondary; elide: Text.ElideRight
                    text: Documents.busy ? "正在翻译第 " + Math.min(Documents.completed + 1, Documents.total) + " / " + Documents.total + " 段"
                        : Documents.state === "completed" ? "已完成全部 " + Documents.total + " 段"
                        : Documents.state === "paused" || Documents.state === "failed" ? "已保留 " + Documents.completed + " / " + Documents.total + " 段，继续时重译未完成段"
                        : Documents.total ? "准备就绪 · 点击后向所选服务逐段发送原文" : "先导入文件，再开始翻译"
                }
                UiText { Layout.fillWidth: true; visible: Documents.taskDescription.length > 0; text: Documents.taskDescription; font.pixelSize: 10; color: Theme.muted; elide: Text.ElideRight }
            }
            UiButton { objectName: "documentResetButton"; text: "重新翻译"; kind: "ghost"; compact: true; visible: Documents.taskDescription.length > 0; enabled: !Documents.busy; onClicked: Documents.reset() }
            UiButton {
                objectName: "documentExportButton"; iconName: "download"; compact: true
                text: Documents.state === "completed" ? "导出 TXT" : "导出已完成部分"
                enabled: !Documents.busy && Documents.completed > 0
                onClicked: { saveFile.selectedFile = Documents.suggestedOutput; saveFile.open() }
            }
            UiButton {
                objectName: "documentTranslateButton"; kind: "primary"; compact: true
                text: Documents.busy ? "停止" : Documents.state === "paused" || Documents.state === "failed" ? "继续翻译" : "开始翻译"
                iconName: Documents.busy ? "stop" : "arrowRight"
                enabled: Documents.total > 0 && Documents.state !== "completed" && !App.busy && !App.testBusy && !App.visionBusy && !App.pdfBusy && (!App.documentBusy || Documents.busy)
                onClicked: {
                    if (Documents.busy) Documents.cancel()
                    else if (Documents.state === "paused" || Documents.state === "failed") Documents.resume()
                    else Documents.start()
                }
            }
        }
    }
    UiText {
        Layout.fillWidth: true; font.pixelSize: 10; color: Theme.muted; wrapMode: Text.Wrap
        text: "翻译使用所选服务；续译沿用本次任务的模型和语言。关闭程序前请导出已完成部分。"
    }
}
