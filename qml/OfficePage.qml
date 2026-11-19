import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ColumnLayout {
    id: page
    signal openSettings()
    property int selectedIndex: -1
    property var selected: selectedIndex >= 0 && selectedIndex < Office.segments.length ? Office.segments[selectedIndex] : ({})
    readonly property bool fileDialogOpen: openFile.visible || saveFile.visible || rendererFile.visible
    readonly property bool locked: Office.busy || Office.processing
    readonly property bool translationComplete: Office.loaded && Office.total > 0 && Office.completed >= Office.total
    readonly property bool canStartTranslation: !locked && !App.busy && !App.documentBusy && !App.visionBusy && !App.pdfBusy && !App.testBusy
    readonly property var languages: ["简体中文", "繁體中文", "英语", "日语", "韩语", "法语", "德语", "西班牙语", "俄语", "葡萄牙语", "意大利语", "阿拉伯语"]
    readonly property string actualSource: Office.taskSettings.source || App.sourceLanguage
    readonly property string actualTarget: Office.taskSettings.target || App.targetLanguage
    readonly property string actualTone: Office.taskSettings.tone || App.tone
    readonly property string actualProviderId: Office.taskSettings.providerId || App.selectedProviderId
    readonly property string actualProviderName: Office.taskSettings.providerName || App.providerName
    property var pendingSettings: null
    property var sourceEditor: ({ text: "" })
    property var translationEditor: ({ text: "" })
    property bool syncingEditors: false
    function refreshEditors() {
        syncingEditors = true
        var records = Office.segments
        var item = selectedIndex >= 0 && selectedIndex < records.length ? records[selectedIndex] : ({})
        sourceEditor.text = item.source || ""
        var value = Office.draftId === item.id ? Office.draftText : item.translation || ""
        if (translationEditor.text !== value) translationEditor.text = value
        syncingEditors = false
    }
    function selectSegment(index) { selectedIndex = index; refreshEditors() }
    function requestSelection(index) {
        if (locked || index === selectedIndex) return
        if (Office.draftDirty) { discardDialog.nextIndex = index; discardDialog.open() }
        else { selectSegment(index); segmentList.positionViewAtIndex(index, ListView.Contain) }
    }
    function restoreSettingBindings() {
        sourceLanguage.currentIndex = Qt.binding(function() { return sourceLanguage.model.indexOf(page.actualSource) })
        targetLanguage.currentIndex = Qt.binding(function() { return targetLanguage.model.indexOf(page.actualTarget) })
        toneChoice.currentIndex = Qt.binding(function() { return toneChoice.model.indexOf(page.actualTone) })
        providerChoice.currentIndex = Qt.binding(function() {
            for (var i = 0; i < App.providers.length; ++i) if (App.providers[i].id === page.actualProviderId) return i
            return -1
        })
    }
    function applySettings(settings) {
        if (locked || !settings) return false
        if (Office.loaded && !Office.resetTranslations()) return false
        App.sourceLanguage = settings.source
        App.targetLanguage = settings.target
        App.tone = settings.tone
        App.selectProvider(settings.providerId)
        restoreSettingBindings()
        return true
    }
    function visibleSettings() {
        return { source: actualSource, target: actualTarget, tone: actualTone, providerId: actualProviderId }
    }
    function requestSetting(key, value) {
        var settings = visibleSettings()
        if (!locked && settings[key] !== value) {
            settings[key] = value
            settings.providerName = actualProviderName
            for (var i = 0; i < App.providers.length; ++i) if (App.providers[i].id === settings.providerId) { settings.providerName = App.providers[i].name; break }
            if (Office.taskSettings.target || Office.completed > 0 || Office.draftDirty) {
                pendingSettings = settings
                settingsChange.open()
            } else applySettings(settings)
        }
        restoreSettingBindings()
    }
    spacing: 9
    FileDialog {
        id: openFile; objectName: "officeOpenDialog"; title: "打开 Word / PPT"
        nameFilters: ["Office 文档 (*.docx *.pptx)", "Word 文档 (*.docx)", "PowerPoint 演示文稿 (*.pptx)"]
        onAccepted: FileImport.requestOpen(selectedFile)
    }
    FileDialog {
        id: saveFile; objectName: "officeSaveDialog"; title: "另存译文"
        fileMode: FileDialog.SaveFile; defaultSuffix: Office.kind
        nameFilters: Office.kind === "pptx" ? ["PowerPoint 演示文稿 (*.pptx)"] : ["Word 文档 (*.docx)"]
        onAccepted: Office.saveFile(selectedFile)
    }
    FileDialog { id: rendererFile; title: "选择 LibreOffice 程序"; nameFilters: ["LibreOffice (soffice.exe)"]; onAccepted: Office.rendererPath = selectedFile }
    RowLayout {
        Layout.fillWidth: true
        ColumnLayout {
            Layout.fillWidth: true; spacing: 3
            UiText { text: "Word / PPT 翻译"; font.pixelSize: 22; font.weight: Font.DemiBold }
            UiText { text: Office.fileName || "保留文档结构，另存为可编辑的译文。"; textFormat: Text.PlainText; elide: Text.ElideMiddle; Layout.fillWidth: true; font.pixelSize: 12; color: Theme.secondary }
        }
        UiButton { objectName: "officeImportButton"; text: Office.loaded ? "更换文档" : "打开文档"; iconName: "document"; kind: "soft"; compact: true; enabled: !page.locked; onClicked: openFile.open() }
        UiButton { objectName: "officeImagesButton"; text: "图片取字"; compact: true; visible: Office.images.length > 0; enabled: !page.locked && !Capture.busy && !Vision.busy; onClicked: imagesDialog.open() }
        UiButton { objectName: "officeOptionsButton"; iconName: "settings"; hint: "文档与版式选项"; compact: true; onClicked: options.open() }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 6
        UiCombo { id: sourceLanguage; objectName: "officeSourceLanguage"; model: ["自动检测"].concat(page.languages); currentIndex: model.indexOf(page.actualSource); onActivated: page.requestSetting("source", currentText); Layout.preferredWidth: 112; enabled: !page.locked }
        Icon { name: "arrowRight"; color: Theme.muted; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
        UiCombo { id: targetLanguage; objectName: "officeTargetLanguage"; model: page.languages; currentIndex: model.indexOf(page.actualTarget); onActivated: page.requestSetting("target", currentText); Layout.preferredWidth: 105; enabled: !page.locked }
        UiCombo { id: toneChoice; objectName: "officeTone"; model: ["自然", "正式", "口语", "学术"]; currentIndex: model.indexOf(page.actualTone); onActivated: page.requestSetting("tone", currentText); Layout.preferredWidth: 80; enabled: !page.locked }
        Item { Layout.fillWidth: true }
        UiCombo {
            id: providerChoice; objectName: "officeProvider"; model: App.providers; textRole: "name"; Layout.preferredWidth: 142
            displayText: page.actualProviderName || "选择 AI 服务"
            currentIndex: { for (var i = 0; i < App.providers.length; ++i) if (App.providers[i].id === page.actualProviderId) return i; return -1 }
            enabled: !page.locked && App.providers.length > 0; onActivated: page.requestSetting("providerId", App.providers[currentIndex].id)
        }
    }
    Rectangle {
        visible: Office.error.length > 0; Layout.fillWidth: true; implicitHeight: errorText.implicitHeight + 16; radius: 8; color: Theme.dangerSoft
        UiText { id: errorText; objectName: "officeError"; anchors.fill: parent; anchors.margins: 8; text: Office.error; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.danger; font.pixelSize: 11 }
    }
    RowLayout {
        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
        Rectangle {
            Layout.preferredWidth: 190; Layout.minimumWidth: 145; Layout.fillHeight: true; radius: 10; color: Theme.surface; border.color: Theme.border
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 10; spacing: 6
                UiText { text: Office.kind === "pptx" ? Office.pageCount + " 页 · " + Office.total + " 段" : "文档段落 · " + Office.total; font.pixelSize: 11; color: Theme.secondary }
                ListView {
                    id: segmentList; objectName: "officeSegmentList"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: Office.segments; spacing: 4; ScrollBar.vertical: UiScrollBar {}
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: segmentList.width; height: 63; radius: 7
                        color: page.selectedIndex === index ? Theme.accentSoft : mouse.containsMouse ? Theme.hover : "transparent"
                        Column {
                            anchors.fill: parent; anchors.margins: 7; spacing: 4
                            UiText { width: parent.width; text: (modelData.kept ? "原文 · " : modelData.done ? "已译 · " : modelData.failure ? "失败 · " : "待译 · ") + modelData.location; textFormat: Text.PlainText; elide: Text.ElideRight; font.pixelSize: 10; color: modelData.failure ? Theme.danger : Theme.muted }
                            UiText { width: parent.width; text: modelData.source.replace(/\s+/g, " "); textFormat: Text.PlainText; elide: Text.ElideRight; font.pixelSize: 12 }
                        }
                        MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: page.requestSelection(index) }
                    }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
            UiText { Layout.fillWidth: true; text: page.selected.location || "选择段落查看原文与译文"; textFormat: Text.PlainText; elide: Text.ElideMiddle; font.pixelSize: 11; color: Theme.secondary }
            Repeater {
                model: [false, true]
                delegate: Rectangle {
                    required property bool modelData
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredHeight: 1; radius: 10; color: Theme.surface; border.color: Theme.border
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 12; spacing: 6
                        UiText { text: modelData ? "译文 · 可校对编辑" : "原文"; color: Theme.secondary; font.pixelSize: 11 }
                        UiScrollView {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            TextArea {
                                id: paragraphEditor
                                objectName: modelData ? "officeTranslationEditor" : "officeSourceEditor"
                                readOnly: !modelData || page.locked; selectByMouse: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                                color: Theme.text; placeholderTextColor: Theme.muted; selectionColor: Theme.accent; selectedTextColor: Theme.accentText
                                font.family: Theme.font; font.pixelSize: 14; background: null; padding: 0
                                placeholderText: modelData ? "开始翻译后显示译文，也可自行填写。" : "打开或拖入 DOCX / PPTX 文档。"
                                onTextChanged: { if (modelData && activeFocus && !page.syncingEditors && !page.locked && page.selectedIndex >= 0) Office.stageEdit(page.selected.id, text) }
                                Component.onCompleted: { if (modelData) page.translationEditor = paragraphEditor; else page.sourceEditor = paragraphEditor }
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 5
                UiButton { objectName: "officeApplyEdit"; text: "保存校对"; compact: true; kind: "soft"; enabled: !page.locked && page.selectedIndex >= 0; onClicked: Office.editSegment(page.selected.id, translationEditor.text) }
                UiButton { objectName: "officeKeepOriginal"; text: "保留原文"; compact: true; enabled: !page.locked && page.selectedIndex >= 0; onClicked: Office.keepOriginal(page.selected.id) }
                UiButton { objectName: "officeRetrySegment"; text: "重译此段"; compact: true; enabled: !page.locked && page.selectedIndex >= 0; onClicked: { if (Office.draftDirty) retryDirtyDialog.open(); else Office.retrySegment(page.selected.id) } }
                Item { Layout.fillWidth: true }
                UiButton { objectName: "officeIssuesButton"; text: "校对提示 " + Office.issues.length; compact: true; kind: "ghost"; onClicked: issuesDialog.open() }
            }
        }
    }
    ProgressBar {
        Layout.fillWidth: true; from: 0; to: Math.max(1, Office.total); value: Office.completed
        background: Rectangle { implicitHeight: 4; radius: 2; color: Theme.border }
        contentItem: Item { implicitHeight: 4; Rectangle { width: parent.width * parent.parent.visualPosition; height: parent.height; radius: 2; color: Theme.accent } }
    }
    RowLayout {
        Layout.fillWidth: true
        ColumnLayout {
            Layout.fillWidth: true; spacing: 3
            UiText { objectName: "officeStatus"; Layout.fillWidth: true; text: Office.status; font.pixelSize: 11; color: Theme.secondary; elide: Text.ElideRight }
            UiText { Layout.fillWidth: true; text: Office.taskDescription || "打开不会自动调用模型；译文另存，原文保留。"; font.pixelSize: 10; color: Theme.muted; elide: Text.ElideRight }
        }
        UiButton { objectName: "officeExportButton"; text: "另存译文"; iconName: "download"; compact: true; enabled: Office.loaded && !page.locked; onClicked: { saveFile.selectedFile = Office.suggestedOutput; saveFile.open() } }
        UiButton {
            objectName: "officeTranslateButton"; text: Office.busy ? "暂停" : page.translationComplete ? "重新翻译" : Office.completed ? "继续翻译" : "开始翻译"; kind: "primary"; compact: true
            enabled: !Office.processing && Office.loaded && Office.total > 0 && (Office.busy || page.canStartTranslation)
            onClicked: { if (Office.busy) Office.cancel(); else if (page.translationComplete) restartDialog.open(); else if (translationEditor.text !== (page.selected.translation || "")) startDirtyDialog.open(); else Office.start() }
        }
    }
    Dialog {
        id: restartDialog; objectName: "officeRestartDialog"; parent: Overlay.overlay; anchors.centerIn: parent; modal: true
        title: "重新翻译整份文档？"; width: Math.min(500, parent.width - 40)
        contentItem: ColumnLayout {
            spacing: 14
            UiText { Layout.fillWidth: true; text: "当前设置：" + page.actualProviderName + " · " + page.actualSource + " → " + page.actualTarget + " · " + page.actualTone; wrapMode: Text.Wrap; font.pixelSize: 12 }
            UiText { Layout.fillWidth: true; text: "确认后将清除当前译文和校对草稿，按界面显示的设置从第一段重新翻译。需要保留现有结果时，请先取消并另存译文。"; wrapMode: Text.Wrap; color: Theme.secondary; font.pixelSize: 12 }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { objectName: "officeRestartCancel"; text: "取消"; compact: true; onClicked: restartDialog.close() }
                UiButton { objectName: "officeRestartConfirm"; text: "确认并重译"; kind: "primary"; compact: true; enabled: page.canStartTranslation; onClicked: { if (page.canStartTranslation && page.applySettings(page.visibleSettings())) { restartDialog.close(); Office.start() } } }
            }
        }
    }
    Dialog {
        id: settingsChange; objectName: "officeSettingsChangeDialog"; parent: Overlay.overlay; anchors.centerIn: parent; modal: true
        title: "更改翻译设置？"; width: Math.min(500, parent.width - 40)
        onClosed: { page.pendingSettings = null; page.restoreSettingBindings() }
        contentItem: ColumnLayout {
            spacing: 14
            UiText { Layout.fillWidth: true; text: "当前任务：" + page.actualProviderName + " · " + page.actualSource + " → " + page.actualTarget + " · " + page.actualTone; wrapMode: Text.Wrap; font.pixelSize: 12 }
            UiText { Layout.fillWidth: true; text: page.pendingSettings ? "切换为：" + page.pendingSettings.providerName + " · " + page.pendingSettings.source + " → " + page.pendingSettings.target + " · " + page.pendingSettings.tone : ""; wrapMode: Text.Wrap; font.pixelSize: 12 }
            UiText { Layout.fillWidth: true; text: "切换设置将清除当前文档的已有译文和校对草稿。需要保留时，请先取消并另存译文。切换后点击“开始翻译”，按新设置重新翻译。"; wrapMode: Text.Wrap; color: Theme.secondary; font.pixelSize: 12 }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { objectName: "officeSettingsChangeCancel"; text: "取消"; compact: true; onClicked: settingsChange.close() }
                UiButton { objectName: "officeSettingsChangeConfirm"; text: "清除译文并切换"; kind: "danger"; compact: true; onClicked: { if (page.applySettings(page.pendingSettings)) settingsChange.close() } }
            }
        }
    }
    Dialog {
        id: imagesDialog; objectName: "officeImagesDialog"; parent: Overlay.overlay; anchors.centerIn: parent; modal: true
        title: "文档中的图片"; width: Math.min(500, parent.width - 40); height: Math.min(420, parent.height - 60); standardButtons: Dialog.Close
        contentItem: UiScrollView {
            ColumnLayout {
                width: parent.width; spacing: 10
                UiText { Layout.fillWidth: true; text: "选择图片后手动识别，可复制原文或单独翻译。图片取字不会改写文档中的原图。"; wrapMode: Text.Wrap; font.pixelSize: 12; color: Theme.secondary }
                Repeater {
                    model: Office.images
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        UiText { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; elide: Text.ElideMiddle; font.pixelSize: 12 }
                        UiButton { objectName: "officeReviewImage_" + index; text: "识图取字"; compact: true; onClicked: { var id = modelData.id; imagesDialog.close(); Qt.callLater(function() { Office.reviewImage(id) }) } }
                    }
                }
            }
        }
    }
    Dialog {
        id: discardDialog; objectName: "officeDiscardDialog"; property int nextIndex: -1; parent: Overlay.overlay; anchors.centerIn: parent; modal: true; title: "当前校对尚未保存"
        width: Math.min(460, parent.width - 40)
        standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
        onAccepted: { if (Office.editSegment(page.selected.id, translationEditor.text)) page.selectSegment(nextIndex) }
        onDiscarded: { Office.discardDraft(); page.selectSegment(nextIndex) }
        contentItem: UiText { text: "保存当前译文后切换，或放弃本次编辑。" }
    }
    Dialog {
        id: startDirtyDialog; parent: Overlay.overlay; anchors.centerIn: parent; modal: true; title: "先保存当前校对"
        width: Math.min(460, parent.width - 40)
        standardButtons: Dialog.Save | Dialog.Cancel
        onAccepted: { if (Office.editSegment(page.selected.id, translationEditor.text)) Office.start() }
        contentItem: UiText { text: "保存当前译文后继续翻译。" }
    }
    Dialog {
        id: retryDirtyDialog; parent: Overlay.overlay; anchors.centerIn: parent; modal: true; title: "放弃当前校对并重译？"
        width: Math.min(460, parent.width - 40)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: { Office.discardDraft(); Office.retrySegment(page.selected.id) }
        contentItem: UiText { text: "已有保存的译文会在重译成功后才被替换。" }
    }
    Dialog {
        id: options; parent: Overlay.overlay; anchors.centerIn: parent; modal: true; title: "文档与版式选项"
        width: Math.min(500, parent.width - 40); standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            spacing: 12
            CheckBox { text: "允许 PPT 译文在下限内缩小字号"; checked: Office.fitText; enabled: !page.locked; onToggled: Office.fitText = checked }
            RowLayout {
                UiText { text: "最小字号" }
                SpinBox { from: 6; to: 36; value: Office.minimumFont; enabled: !page.locked; onValueModified: Office.minimumFont = value }
            }
            UiText { Layout.fillWidth: true; text: "Word 可能重新分页。文本框溢出为估算，请检查实际版式。"; wrapMode: Text.Wrap; color: Theme.secondary; font.pixelSize: 12 }
            UiButton { text: "选择 LibreOffice（可选）"; compact: true; enabled: !page.locked; onClicked: rendererFile.open() }
            UiText { Layout.fillWidth: true; text: Office.rendererPath || "未设置；原格式翻译和导出无需安装 Office。"; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.pixelSize: 11; color: Theme.muted }
            RowLayout {
                UiButton { text: "生成 PDF 版式预览"; compact: true; enabled: Office.loaded && !page.locked; onClicked: { Office.generateLayoutPreview(); options.close() } }
                UiButton { text: "打开预览"; compact: true; enabled: Office.layoutPreview.toString().length > 0; onClicked: Office.openLayoutPreview() }
            }
            UiButton { text: "清除译文，使用当前服务重译"; kind: "danger"; compact: true; enabled: Office.loaded && !page.locked; onClicked: { resetDialog.open(); options.close() } }
        }
    }
    Dialog { id: resetDialog; parent: Overlay.overlay; anchors.centerIn: parent; modal: true; title: "清除当前文档译文？"; width: Math.min(460, parent.width - 40); standardButtons: Dialog.Ok | Dialog.Cancel; onAccepted: Office.resetTranslations(); contentItem: UiText { text: "建议先另存译文。清除后将使用当前模型和语言。"; wrapMode: Text.Wrap } }
    Dialog {
        id: issuesDialog; objectName: "officeIssuesDialog"; parent: Overlay.overlay; anchors.centerIn: parent; modal: true
        title: "校对提示"; width: Math.min(640, parent.width - 40); height: Math.min(500, parent.height - 60); standardButtons: Dialog.Close
        contentItem: UiScrollView {
            ColumnLayout {
                width: parent.width; spacing: 12
                Repeater {
                    model: Office.issues
                    delegate: ColumnLayout {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true; spacing: 4
                        UiText { Layout.fillWidth: true; text: modelData.location; textFormat: Text.PlainText; color: Theme.accent; font.pixelSize: 12; wrapMode: Text.Wrap }
                        UiText { Layout.fillWidth: true; text: modelData.message; textFormat: Text.PlainText; wrapMode: Text.Wrap; font.pixelSize: 12 }
                        UiButton { objectName: "officeIssueLocate_" + index; visible: !!modelData.id; text: "定位段落"; compact: true; onClicked: { var id = modelData.id; issuesDialog.close(); Qt.callLater(function() { for (var i = 0; i < Office.segments.length; ++i) if (Office.segments[i].id === id) { page.requestSelection(i); break } }) } }
                    }
                }
                UiText { visible: Office.issues.length === 0; text: "暂无提示，建议导出后检查实际文档。"; color: Theme.secondary }
            }
        }
    }
    Connections {
        target: Office
        function onOpened() {
            var index = Office.total > 0 ? 0 : -1
            for (var i = 0; i < Office.segments.length; ++i) if (Office.segments[i].id === Office.draftId) { index = i; break }
            page.selectSegment(index)
        }
        function onChanged() {
            if (Office.busy && Office.activeIndex >= 0) { page.selectedIndex = Office.activeIndex; page.refreshEditors(); segmentList.positionViewAtIndex(page.selectedIndex, ListView.Contain) }
            else if (page.selectedIndex >= 0) page.refreshEditors()
        }
        function onPreviewChanged() { if (Office.busy && page.selectedIndex === Office.activeIndex) translationEditor.text = Office.preview }
    }
}
