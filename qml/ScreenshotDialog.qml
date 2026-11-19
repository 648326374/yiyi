import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "screenshotDialog"
    parent: Overlay.overlay
    width: Math.min(820, parent.width - 40); height: Math.min(600, parent.height - 40)
    anchors.centerIn: parent
    padding: 20; modal: true
    enter: null
    exit: null
    closePolicy: Popup.CloseOnEscape
    property bool syncingEditor: false
    readonly property bool visionMode: Capture.mode === "vision"
    readonly property bool recognizing: Capture.busy || Vision.busy
    readonly property string draftText: visionMode ? Vision.resultText : Capture.text
    readonly property bool aiAvailable: !App.busy && !App.documentBusy && !App.testBusy && !Vision.busy && !App.pdfBusy
    function syncEditor() {
        if (editor && editor.text !== draftText) {
            syncingEditor = true
            editor.text = draftText
            syncingEditor = false
        }
    }
    onDraftTextChanged: syncEditor()
    Component.onCompleted: syncEditor()
    background: Rectangle { color: Theme.surface; radius: 14; border.color: Theme.border }
    onOpened: syncEditor()
    // Clear while the popup's controls are alive, before Qt detaches its tree.
    onAboutToHide: { Vision.cancel(); if (!Capture.capturing) Capture.clearImage() }
    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Icon { name: "screenshot"; color: Theme.accent; Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
            UiText { text: "图片取字"; font.pixelSize: 19; font.weight: Font.DemiBold }
            Item { Layout.fillWidth: true }
            UiButton { iconName: "close"; hint: "关闭并清除截图"; kind: "ghost"; compact: true; onClicked: dialog.close() }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            UiCombo {
                objectName: "screenshotMode"; Layout.preferredWidth: 180
                model: ["本地 OCR", "AI 识图取字"]
                currentIndex: dialog.visionMode ? 1 : 0; enabled: !dialog.recognizing
                onActivated: Capture.mode = currentIndex === 1 ? "vision" : "local"
            }
            UiText { Layout.fillWidth: true; text: "识别原文，校对或复制后再翻译"; color: Theme.secondary; font.pixelSize: 11; elide: Text.ElideRight }
            UiButton {
                objectName: "ocrRecaptureButton"; text: "重新截图"; iconName: "screenshot"; compact: true; enabled: !Capture.busy && !Vision.busy
                onClicked: { dialog.close(); Qt.callLater(function() { Capture.capture() }) }
            }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            UiText {
                visible: !dialog.visionMode; Layout.fillWidth: true
                text: "RapidOCR · 中英文 · 支持深色背景与小字预处理"
                color: Theme.secondary; font.pixelSize: 11; elide: Text.ElideRight
            }
            UiText { visible: dialog.visionMode; text: "识图模型"; color: Theme.secondary; font.pixelSize: 11 }
            UiCombo {
                objectName: "visionProvider"; visible: dialog.visionMode; Layout.fillWidth: true; model: App.providers; textRole: "name"
                currentIndex: { for (var i = 0; i < model.length; ++i) if (model[i].id === Vision.providerId) return i; return -1 }
                displayText: currentIndex >= 0 ? currentText : "请先在偏好设置添加视觉模型"; enabled: !dialog.recognizing
                onActivated: Vision.providerId = model[currentIndex].id
            }
            UiButton {
                objectName: "screenshotRecognizeButton"; text: dialog.draftText.length > 0 ? "重新识别" : "开始识别"; compact: true
                enabled: Capture.hasImage && !dialog.recognizing && (!dialog.visionMode || dialog.aiAvailable)
                onClicked: { if (dialog.visionMode) Vision.recognize(); else Capture.recognize() }
            }
        }
        UiText {
            Layout.fillWidth: true; visible: text.length > 0; text: dialog.visionMode ? Vision.error : Capture.error
            color: Theme.danger; wrapMode: Text.Wrap; font.pixelSize: 11; maximumLineCount: 3; elide: Text.ElideRight
        }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12
            Rectangle {
                Layout.preferredWidth: 210; Layout.fillHeight: true; color: Theme.subtle; radius: 10; border.color: Theme.border
                Image { anchors.fill: parent; anchors.margins: 10; source: Capture.preview; fillMode: Image.PreserveAspectFit; cache: false }
                UiText { anchors.centerIn: parent; visible: !Capture.hasImage; text: "截图预览"; color: Theme.muted; font.pixelSize: 12 }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.subtle; border.color: Theme.border; radius: 10
                UiScrollView {
                    anchors.fill: parent; anchors.margins: 14; clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    TextArea {
                        id: editor; objectName: "ocrEditor"
                        placeholderText: dialog.recognizing ? "正在识别原文…" : "识别后的原文显示在这里，可校对、复制或翻译。"
                        placeholderTextColor: Theme.muted; color: Theme.text
                        textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap; readOnly: dialog.recognizing
                        font.family: Theme.font; font.pixelSize: 15
                        selectByMouse: true; selectionColor: Theme.accent; selectedTextColor: Theme.accentText
                        padding: 0; background: null
                        onTextChanged: {
                            if (!dialog.syncingEditor && !dialog.recognizing) {
                                if (dialog.visionMode) Vision.resultText = text
                                else Capture.text = text
                            }
                        }
                    }
                }
            }
        }
        UiText { Layout.fillWidth: true; text: dialog.visionMode ? Vision.resultInfo : Capture.note; visible: text.length > 0; color: Theme.secondary; font.pixelSize: 10; elide: Text.ElideRight }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            UiText { text: "翻译使用"; color: Theme.secondary; font.pixelSize: 11 }
            UiText {
                objectName: "screenshotTranslationProvider"; Layout.fillWidth: true
                text: App.providerName + (App.modelName.length > 0 ? " · " + App.modelName : "")
                color: Theme.secondary; font.pixelSize: 11; elide: Text.ElideRight
            }
            UiText { text: "译为"; color: Theme.secondary; font.pixelSize: 11 }
            UiCombo {
                objectName: "screenshotTarget"; Layout.preferredWidth: 130
                model: ["简体中文", "繁體中文", "英语", "日语", "韩语", "法语", "德语", "西班牙语", "俄语", "葡萄牙语", "意大利语", "阿拉伯语"]
                currentIndex: model.indexOf(App.targetLanguage); enabled: !dialog.recognizing
                onActivated: App.targetLanguage = currentText
            }
        }
        RowLayout {
            Layout.fillWidth: true
            UiText {
                Layout.fillWidth: true
                text: Vision.busy ? Vision.status : Capture.busy ? "正在本机识别…" : editor.text.length + " 字符 · 可编辑"
                color: Theme.secondary; font.pixelSize: 11; elide: Text.ElideRight
            }
            UiButton { objectName: "screenshotCancelButton"; text: "取消识别"; compact: true; visible: dialog.recognizing; onClicked: { if (dialog.visionMode) Vision.cancel(); else Capture.cancel() } }
            UiButton {
                objectName: "screenshotCopyButton"; text: "复制原文"; compact: true
                enabled: !dialog.recognizing && editor.text.trim().length > 0
                onClicked: { if (dialog.visionMode) Vision.copyResult(); else Capture.copyResult(); App.toast("已复制原文") }
            }
            UiButton { objectName: "ocrUseButton"; text: "放入原文"; compact: true; enabled: !dialog.recognizing && editor.text.trim().length > 0 && dialog.aiAvailable; onClicked: if (App.importText(editor.text)) dialog.close() }
            UiButton { objectName: "ocrTranslateButton"; text: "翻译文字"; compact: true; kind: "primary"; enabled: !dialog.recognizing && editor.text.trim().length > 0 && dialog.aiAvailable; onClicked: if (App.importText(editor.text)) { dialog.close(); App.translate() } }
        }
        UiText {
            Layout.fillWidth: true; font.pixelSize: 10; color: Theme.muted; wrapMode: Text.Wrap
            text: dialog.visionMode ? "点击识别后，截图发送给所选视觉模型；点击翻译时，仅发送校对后的文字。llama.cpp 需加载匹配的 mmproj。"
                : "OCR 在本机完成；点击翻译后，校对过的文字才会发送至当前 AI 服务。"
        }
    }
}
