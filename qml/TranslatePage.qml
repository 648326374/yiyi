import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    signal openSettings()
    signal screenshotRequested()
    readonly property var languages: ["简体中文", "繁體中文", "英语", "日语", "韩语", "法语", "德语", "西班牙语", "俄语", "葡萄牙语", "意大利语", "阿拉伯语"]
    spacing: 16

    RowLayout {
        Layout.fillWidth: true
        ColumnLayout {
            spacing: 5
            UiText { text: "翻译工作台"; font.pixelSize: 22; font.weight: Font.DemiBold }
            UiText { text: "从一个词，到更广阔的世界。"; color: Theme.secondary; font.pixelSize: 12 }
        }
        Item { Layout.fillWidth: true }
        UiButton { objectName: "screenshotButton"; text: Capture.busy ? "正在取字…" : "截图取字"; iconName: "screenshot"; compact: true; enabled: !Capture.busy; onClicked: page.screenshotRequested() }
        Rectangle {
            color: Theme.accentSoft; radius: 15
            implicitWidth: previewLabel.implicitWidth + 28; implicitHeight: 30
            visible: App.example
            UiText { id: previewLabel; text: "示例预览 · 未发送请求"; color: Theme.accent; font.pixelSize: 11; anchors.centerIn: parent }
        }
    }

    RowLayout {
        Layout.fillWidth: true; spacing: 8; Layout.topMargin: 2
        Rectangle {
            implicitWidth: modes.implicitWidth + 8; implicitHeight: 38
            color: Theme.hover; radius: 10
            Row {
                id: modes; anchors.centerIn: parent; spacing: 3
                Repeater {
                    model: [{ label: "智能识别", value: "auto" }, { label: "文本翻译", value: "translation" }, { label: "词典", value: "dictionary" }]
                    delegate: Button {
                        required property var modelData
                        implicitWidth: modelData.value === "dictionary" ? 58 : 80; implicitHeight: 30
                        onClicked: App.mode = modelData.value
                        contentItem: UiText { text: modelData.label; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 12; font.weight: App.mode === modelData.value ? Font.DemiBold : Font.Normal; color: App.mode === modelData.value ? Theme.accent : Theme.secondary }
                        background: Rectangle { radius: 7; color: App.mode === modelData.value ? Theme.surface : "transparent"; border.width: parent.activeFocus ? 1 : 0; border.color: Theme.accent }
                    }
                }
            }
        }
        Item { Layout.fillWidth: true }
        Icon { name: "sparkles"; color: Theme.accent; width: 17; height: 17 }
        UiCombo {
            id: providerSelector
            Layout.preferredWidth: 164
            model: App.providers
            textRole: "name"
            displayText: App.providers.length ? App.providerName : "选择 AI 服务"
            currentIndex: {
                for (var i = 0; i < App.providers.length; ++i) if (App.providers[i].id === App.selectedProviderId) return i
                return -1
            }
            onActivated: App.selectProvider(App.providers[currentIndex].id)
            enabled: App.providers.length > 0 && !App.busy
        }
        UiButton { iconName: "settings"; hint: "管理 AI 服务"; onClicked: page.openSettings(); compact: true; implicitHeight: 40; implicitWidth: 40 }
    }

    Rectangle {
        visible: App.error.length > 0
        Layout.fillWidth: true; implicitHeight: errorRow.implicitHeight + 24
        radius: 9; color: Theme.dangerSoft
        RowLayout {
            id: errorRow
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
            Icon { name: "info"; color: Theme.danger; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
            UiText { text: App.error; color: Theme.danger; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
            UiButton { iconName: "close"; kind: "ghost"; compact: true; hint: "关闭提示"; onClicked: App.clearError() }
        }
    }

    RowLayout {
        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14
        Rectangle {
            objectName: "sourceCard"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 340
            color: Theme.surface; border.color: Theme.border; radius: 12
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                RowLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 6
                    UiText { text: "原文"; color: Theme.secondary; font.pixelSize: 11; Layout.leftMargin: 5 }
                    UiCombo {
                        model: ["自动检测"].concat(page.languages)
                        currentIndex: model.indexOf(App.sourceLanguage)
                        onActivated: App.sourceLanguage = currentText
                        quiet: true; Layout.preferredWidth: 110; implicitHeight: 30
                        enabled: !App.busy
                    }
                    UiButton { iconName: "swap"; kind: "ghost"; compact: true; hint: "交换原文与目标语言"; onClicked: App.swapLanguages(); enabled: !App.busy }
                    Item { Layout.fillWidth: true }
                    UiButton { iconName: "close"; kind: "ghost"; compact: true; hint: "清空输入和结果"; onClicked: App.clear(); enabled: !App.busy }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                UiScrollView {
                    id: inputScroll
                    objectName: "inputScroll"
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Layout.margins: 18; Layout.bottomMargin: 8
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    TextArea {
                        id: input
                        objectName: "sourceInput"
                        text: App.inputText
                        placeholderText: "输入一个单词、短语，\n或粘贴一段想翻译的文字…"
                        placeholderTextColor: Theme.muted
                        color: Theme.text
                        font.family: Theme.font
                        font.pixelSize: 18
                        wrapMode: TextEdit.Wrap
                        padding: 0
                        selectByMouse: true
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.accentText
                        readOnly: App.busy
                        background: null
                        onTextChanged: if (App.inputText !== text) App.inputText = text
                        Accessible.name: "待翻译的原文"
                        Keys.onPressed: function(event) {
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) { App.translate(); event.accepted = true }
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 18; Layout.rightMargin: 18; Layout.bottomMargin: 12
                    spacing: 10
                    Rectangle {
                        visible: App.example && page.height >= 540
                        Layout.fillWidth: true; implicitHeight: exampleColumn.implicitHeight + 24
                        radius: 8; color: Theme.subtle
                        ColumnLayout {
                            id: exampleColumn
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                            spacing: 6
                            RowLayout {
                                Icon { name: "book"; color: Theme.accent; Layout.preferredWidth: 17; Layout.preferredHeight: 17 }
                                UiText { text: "好奇心，是理解的开始"; font.pixelSize: 11; font.weight: Font.DemiBold }
                            }
                            UiText { text: "右侧为词条例子，连接 AI 即可开始查询。"; color: Theme.secondary; font.pixelSize: 11; wrapMode: Text.Wrap; lineHeight: 1.5; Layout.fillWidth: true }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        UiButton { text: "粘贴文本"; iconName: "paste"; compact: true; kind: "ghost"; onClicked: App.pasteInput(); enabled: !App.busy }
                        Item { Layout.fillWidth: true }
                        UiText { text: App.inputText.length.toLocaleString(Qt.locale("en_US"), "f", 0) + " / 24,000"; color: App.inputText.length > 24000 ? Theme.danger : Theme.muted; font.pixelSize: 10 }
                    }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 16; spacing: 10
                    RowLayout {
                        Layout.fillWidth: true
                        UiText { text: "翻译风格"; color: Theme.secondary; font.pixelSize: 11 }
                        UiCombo { model: ["自然", "正式", "口语", "学术"]; currentIndex: model.indexOf(App.tone); onActivated: App.tone = currentText; Layout.preferredWidth: 95; implicitHeight: 32; quiet: true; enabled: !App.busy && App.mode !== "dictionary" }
                        Item { Layout.fillWidth: true }
                        UiText { text: "Ctrl + Enter"; color: Theme.muted; font.family: "Segoe UI"; font.pixelSize: 11 }
                    }
                    UiButton {
                        objectName: "translateButton"
                        enabled: !App.documentBusy && !App.pdfBusy && !App.visionBusy && !App.testBusy
                        Layout.fillWidth: true
                        implicitHeight: 42
                        text: App.busy ? "停止生成" : App.mode === "dictionary" ? "查询词条" : "开始翻译"
                        iconName: App.busy ? "stop" : "arrowRight"
                        kind: "primary"
                        onClicked: App.busy ? App.cancel() : App.translate()
                    }
                }
            }
        }

        Rectangle {
            objectName: "resultCard"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 440
            color: Theme.surface; border.color: Theme.border; radius: 12
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                RowLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 6
                    UiText { text: App.resultKind === "dictionary" ? (App.briefResult ? "释义" : "词条") : "译文"; color: Theme.secondary; font.pixelSize: 11; Layout.leftMargin: 5 }
                    UiCombo {
                        model: page.languages
                        currentIndex: model.indexOf(App.targetLanguage)
                        onActivated: App.targetLanguage = currentText
                        quiet: true; Layout.preferredWidth: 108; implicitHeight: 30; enabled: !App.busy
                    }
                    Item { Layout.fillWidth: true }
                    UiButton { objectName: "copyResultButton"; iconName: "copy"; kind: "ghost"; compact: true; hint: "复制完整结果"; enabled: App.resultText.length > 0; onClicked: App.copyResult() }
                    UiButton { objectName: "favoriteButton"; iconName: App.favorite ? "check" : "star"; kind: App.favorite ? "soft" : "ghost"; compact: true; hint: App.example ? "示例仅供预览，查询后可收藏" : App.favorite ? "取消收藏" : "收藏到生词本"; enabled: App.canSave; onClicked: App.toggleFavorite() }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                UiText {
                    objectName: "generationStatus"
                    visible: App.busy && resultScroll.visible
                    text: App.generationStatus
                    color: Theme.secondary; font.pixelSize: 11
                    Layout.fillWidth: true; Layout.leftMargin: 20; Layout.rightMargin: 16; Layout.topMargin: 10
                }
                UiScrollView {
                    id: resultScroll
                    objectName: "resultScroll"
                    visible: App.resultText.length > 0 || Object.keys(App.dictionary).length > 0
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Layout.margins: 20; Layout.rightMargin: 16
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    Column {
                        width: resultScroll.availableWidth
                        spacing: 0
                        DictionaryView { width: parent.width - 6; visible: App.resultKind === "dictionary"; entry: App.dictionary; brief: App.briefResult }
                        TextArea {
                            visible: App.resultKind !== "dictionary"
                            width: parent.width - 6
                            text: App.resultText
                            color: Theme.text
                            font.family: Theme.font; font.pixelSize: 16
                            textFormat: TextEdit.PlainText
                            wrapMode: TextEdit.Wrap; readOnly: true; selectByMouse: true
                            selectionColor: Theme.accent; selectedTextColor: Theme.accentText
                            padding: 0; background: null
                        }
                        Item { width: 1; height: 10 }
                    }
                }
                Item {
                    visible: !resultScroll.visible
                    Layout.fillWidth: true; Layout.fillHeight: true
                    ColumnLayout {
                        width: Math.min(280, parent.width - 40); anchors.centerIn: parent; spacing: 12
                        Image { source: "qrc:/assets/empty.svg"; sourceSize.width: 288; sourceSize.height: 224; Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 144; Layout.preferredHeight: 112; opacity: Theme.dark ? 0.65 : 1; visible: !App.busy }
                        BusyIndicator { running: App.busy; visible: App.busy; Layout.alignment: Qt.AlignHCenter; palette.dark: Theme.accent }
                        UiText { text: App.busy ? "正在生成…" : "让理解，在这里发生"; font.pixelSize: 17; Layout.alignment: Qt.AlignHCenter }
                        UiText { text: App.busy ? App.generationStatus : "输入文字并开始翻译，\n你的译文和词条就会出现在这里。"; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; color: Theme.secondary; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.6 }
                        UiButton { text: "浏览词条例子"; kind: "ghost"; compact: true; visible: !App.busy; Layout.alignment: Qt.AlignHCenter; onClicked: App.showExample() }
                    }
                }
                UiText {
                    objectName: "requestTiming"
                    text: App.requestTiming; visible: text.length > 0
                    color: Theme.secondary; font.pixelSize: 10
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.bottomMargin: 8
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                RowLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 7
                    Icon { name: App.example ? "info" : "sparkles"; color: Theme.muted; Layout.preferredWidth: 13; Layout.preferredHeight: 13 }
                    UiText { text: App.example ? "内置示例，用于展示词典排版" : App.resultProvider.length ? App.resultProvider + (App.resultModel.length ? " · " + App.resultModel : "") : "由你选择的 AI 服务提供结果"; color: Theme.muted; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                    UiText { text: App.busy ? "生成中…" : App.example ? "" : App.resultText.length ? "AI 生成 · 仅供参考" : ""; color: Theme.muted; font.pixelSize: 10 }
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 6
        Icon { name: "shield"; color: Theme.muted; Layout.preferredWidth: 13; Layout.preferredHeight: 13 }
        UiText { text: "历史与生词本保存在本机"; color: Theme.muted; font.pixelSize: 10 }
        Item { Layout.fillWidth: true }
        UiText { text: "智能识别会根据输入长度，在查词与翻译间自动切换"; color: Theme.muted; font.pixelSize: 10 }
    }
}
