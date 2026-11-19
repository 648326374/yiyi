import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ColumnLayout {
    id: page
    signal openSettings()
    signal toggleReading()
    property bool expanded: false
    property real zoom: 1
    property bool textView: false
    property int scanTab: 0
    property bool syncingScan: false
    property int lastPage: -1
    property bool wasRecognizing: false
    readonly property int recognitionPixels: settingsDialog.recognitionPixels
    readonly property bool fileDialogOpen: openFile.visible || saveFile.visible
    function syncScan() {
        var value = scanTab === 0 ? Pdf.scanSource : Pdf.scanTranslation
        if (scanEditor.text !== value) { syncingScan = true; scanEditor.text = value; syncingScan = false }
    }
    onScanTabChanged: syncScan()
    function recognizeCurrent() {
        if (Pdf.scanSource.length > 0 || !Pdf.scanMode) recognizeNotice.open()
        else Pdf.recognizePages(Pdf.page + 1, Pdf.page + 1, recognitionPixels, true)
    }
    readonly property bool canTranslate: !App.busy && !App.documentBusy && !App.visionBusy && !App.testBusy
    readonly property int scopeTotal: scope.currentIndex === 1 ? Pdf.pageTotal : Pdf.total
    readonly property int scopeCompleted: scope.currentIndex === 1 ? Pdf.pageCompleted : Pdf.completed
    readonly property bool scopeReady: scope.currentIndex === 1 ? Pdf.pageReady : Pdf.analysisComplete
    property string focusedBlockId: ""
    readonly property bool needsRecognition: scope.currentIndex === 1 ? Pdf.scanMode && !Pdf.layoutReady : Pdf.scanPages > Pdf.recognizedPages
    function focusBlock(id) {
        var items = Pdf.blocks
        for (var i = 0; i < items.length; ++i) if (items[i].id === id) {
            page.textView = false; page.focusedBlockId = id
            var sheetWidth = Math.max(120, (viewer.width - 12) / 2) * page.zoom
            viewer.contentY = Math.max(0, Math.min(viewer.contentHeight - viewer.height, items[i].region[1] / Pdf.pageWidth * sheetWidth - 30))
            viewer.contentX = Math.max(0, viewer.contentWidth - viewer.width)
            editorDialog.edit(items[i]); return
        }
    }
    spacing: 9

    FileDialog {
        id: openFile; objectName: "pdfOpenDialog"; title: "打开 PDF"; nameFilters: ["PDF 文档 (*.pdf)"]
        onAccepted: FileImport.requestOpen(selectedFile)
    }
    FileDialog {
        id: saveFile; title: "导出译文 PDF"; fileMode: FileDialog.SaveFile
        nameFilters: ["PDF 文档 (*.pdf)"]; defaultSuffix: "pdf"
        onAccepted: Pdf.exportFile(selectedFile, true)
    }
    function chooseOutput() { saveFile.selectedFile = Pdf.suggestedOutput; saveFile.open() }
    PdfStructureDialog { id: structureDialog; onEditBlockRequested: function(block) { editorDialog.edit(block) } }
    Dialog {
        id: partialNotice; parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(460, parent.width - 40); modal: true; padding: 20
        background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 16
            UiText { text: "导出当前进度"; font.pixelSize: 18; font.weight: Font.DemiBold }
            UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "已完成 " + Pdf.completed + " / " + Pdf.total + " 个翻译项。尚未翻译的内容和 " + Pdf.unsupportedPages + " 个未处理页面保留原文。无法排版或未通过校验的区域也保留原文，并附检查清单；已保存的译文不会删除。" }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "取消"; compact: true; onClicked: partialNotice.close() }
                UiButton { text: "继续导出"; kind: "primary"; compact: true; onClicked: { partialNotice.close(); page.chooseOutput() } }
            }
        }
    }
    Dialog {
        id: exportResult; objectName: "pdfExportResult"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(540, parent.width - 40); modal: true; padding: 20
        property url file
        property url report
        property string details: ""
        background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 16
            UiText { text: "PDF 已导出"; font.pixelSize: 18; font.weight: Font.DemiBold }
            UiText { objectName: "pdfExportResultDetails"; Layout.fillWidth: true; wrapMode: Text.Wrap; text: exportResult.details }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { objectName: "pdfExportViewIssues"; text: "查看待校对"; compact: true; onClicked: { exportResult.close(); issuesDialog.open() } }
                UiButton { text: "打开清单"; compact: true; enabled: exportResult.report.toString().length > 0; onClicked: Qt.openUrlExternally(exportResult.report) }
                UiButton { objectName: "pdfExportResultDone"; text: "完成"; kind: "primary"; compact: true; onClicked: exportResult.close() }
            }
        }
    }
    Connections {
        target: Pdf
        function onExported(file, blocks, pages, report, warning) {
            if (!blocks && !warning) return
            exportResult.file = file; exportResult.report = report
            exportResult.details = pages + " 页中的 " + blocks + " 个区域保留原文，其余可用译文已导出。任务中的译文仍保留，可校对后再次导出。\n\n"
                + (warning || "同目录已保存 .issues.txt 检查清单，列出页码、段落和原因。")
            exportResult.open()
        }
    }
    Dialog {
        id: resetNotice; parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(440, parent.width - 40); modal: true; padding: 20
        background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 16
            UiText { text: "清空全书译文"; font.pixelSize: 18; font.weight: Font.DemiBold }
            UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "清除这份文档已保存的译文和译文修改，下次翻译使用当前服务与语言。已识别、校对的原文会保留。" }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "取消"; compact: true; onClicked: resetNotice.close() }
                UiButton { objectName: "pdfConfirmReset"; text: "清空全书译文"; compact: true; kind: "danger"; onClicked: { resetNotice.close(); Pdf.resetTranslations() } }
            }
        }
    }
    Dialog {
        id: recognizeNotice; parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(450, parent.width - 40); modal: true; padding: 20
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 14
            UiText { text: "AI 重新识别当前页"; font.pixelSize: 18 }
            UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "重新定位文字区域，并由 AI 分段识别原文。整页识别成功后替换，失败或取消保留旧内容。原有文字和译文会保留一份草稿，可在整页文字中复制。" }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "取消"; compact: true; onClicked: recognizeNotice.close() }
                UiButton { objectName: "pdfConfirmRecognition"; text: "开始识别"; compact: true; kind: "primary"; onClicked: { recognizeNotice.close(); Pdf.recognizePages(Pdf.page + 1, Pdf.page + 1, page.recognitionPixels, true) } }
            }
        }
    }
    Dialog {
        id: recognitionRange; objectName: "pdfRecognitionRange"; parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(450, parent.width - 40); modal: true; padding: 20
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 14
            UiText { text: "批量识别扫描页"; font.pixelSize: 18 }
            UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "按顺序识别范围内尚无原文的扫描页。已完成的页会保存，识别结束后可校对原文，再点击翻译。" }
            RowLayout {
                UiText { text: "从" }
                SpinBox { id: rangeFirst; objectName: "pdfRecognitionFirst"; from: 1; to: Math.max(1, Pdf.pageCount); value: 1; editable: true }
                UiText { text: "至" }
                SpinBox { id: rangeLast; objectName: "pdfRecognitionLast"; from: rangeFirst.value; to: Math.max(1, Pdf.pageCount); value: to; editable: true }
                UiText { text: "页" }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "取消"; compact: true; onClicked: recognitionRange.close() }
                UiButton { objectName: "pdfStartRangeRecognition"; text: "开始识别"; kind: "primary"; compact: true; onClicked: { recognitionRange.close(); Pdf.recognizePages(rangeFirst.value, rangeLast.value, page.recognitionPixels) } }
            }
        }
    }
    Dialog {
        id: cropDialog; objectName: "pdfCropDialog"; parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(800, parent.width - 40); height: parent.height - 40; modal: true; padding: 16
        property rect region: Qt.rect(0, 0, 0, 0)
        onOpened: region = Qt.rect(0, 0, 0, 0)
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        contentItem: ColumnLayout {
            UiText { text: "拖动框选文字区域 · 识别结果可复制后粘贴到原文"; font.pixelSize: 13 }
            Image {
                id: cropImage; objectName: "pdfCropImage"; Layout.fillWidth: true; Layout.fillHeight: true
                source: Pdf.originalPreview; fillMode: Image.PreserveAspectFit; cache: false
                Rectangle {
                    x: (cropImage.width - cropImage.paintedWidth) / 2 + cropDialog.region.x * cropImage.paintedWidth
                    y: (cropImage.height - cropImage.paintedHeight) / 2 + cropDialog.region.y * cropImage.paintedHeight
                    width: cropDialog.region.width * cropImage.paintedWidth; height: cropDialog.region.height * cropImage.paintedHeight
                    color: "#30347457"; border.color: Theme.accent; border.width: 2; visible: width > 0 && height > 0
                }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.CrossCursor
                    property point startPoint
                    function point(mouse) { return Qt.point(Math.max(0, Math.min(1, (mouse.x - (width - cropImage.paintedWidth) / 2) / Math.max(1, cropImage.paintedWidth))), Math.max(0, Math.min(1, (mouse.y - (height - cropImage.paintedHeight) / 2) / Math.max(1, cropImage.paintedHeight)))) }
                    onPressed: function(mouse) { startPoint = point(mouse); cropDialog.region = Qt.rect(startPoint.x, startPoint.y, 0, 0) }
                    onPositionChanged: function(mouse) { if (pressed) { var p = point(mouse); cropDialog.region = Qt.rect(Math.min(p.x, startPoint.x), Math.min(p.y, startPoint.y), Math.abs(p.x - startPoint.x), Math.abs(p.y - startPoint.y)) } }
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "取消"; compact: true; onClicked: cropDialog.close() }
                UiButton { objectName: "pdfRecognizeCrop"; text: "识别所选区域"; compact: true; kind: "primary"; enabled: cropDialog.region.width >= .005 && cropDialog.region.height >= .005; onClicked: { var r = cropDialog.region; cropDialog.close(); Pdf.recognizeRegion(r, page.recognitionPixels) } }
            }
        }
    }
    Dialog {
        id: regionResult; objectName: "pdfRegionResult"; parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(620, parent.width - 40); height: Math.min(440, parent.height - 40); modal: true; padding: 20
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        contentItem: ColumnLayout {
            UiText { text: "区域识别结果"; font.pixelSize: 18 }
            UiText { text: "复制后粘贴到原文的对应位置，整页原文已保留。"; color: Theme.secondary; font.pixelSize: 11 }
            UiScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                TextArea { id: regionEditor; objectName: "pdfRegionEditor"; textFormat: TextEdit.PlainText; selectByMouse: true; wrapMode: TextEdit.Wrap; color: Theme.text; font.family: Theme.font; font.pixelSize: 14; background: Rectangle { color: Theme.subtle; radius: 8 } }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "关闭"; compact: true; onClicked: regionResult.close() }
                UiButton { text: "复制原文"; compact: true; kind: "primary"; onClicked: Pdf.copyText(regionEditor.text) }
            }
        }
    }
    Dialog {
        id: oldDraft; parent: Overlay.overlay; anchors.centerIn: parent
        property string customText: ""
        width: Math.min(680, parent.width - 40); height: Math.min(540, parent.height - 40); modal: true; padding: 20
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        contentItem: ColumnLayout {
            UiText { text: "上次识别与译文草稿"; font.pixelSize: 18 }
            UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "重新识别前的内容保留在这里，可选中复制，再粘贴到对应段落。"; color: Theme.secondary; font.pixelSize: 12 }
            UiScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                TextArea { text: oldDraft.customText || Pdf.recognitionDraft; readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap; color: Theme.text; font.family: Theme.font; font.pixelSize: 14; background: Rectangle { color: Theme.subtle; radius: 8 } }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "关闭"; compact: true; onClicked: oldDraft.close() }
                UiButton { text: "复制草稿"; compact: true; onClicked: Pdf.copyText(oldDraft.customText || Pdf.recognitionDraft) }
            }
        }
    }
    Dialog {
        id: editorDialog; objectName: "pdfEditorDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(780, parent.width - 40); height: Math.min(600, parent.height - 40)
        modal: true; padding: height < 580 ? 16 : 20
        property var block: ({})
        property string rawTranslation: ""
        property string displayedTranslation: ""
        function edit(value) {
            block = value; originalEditor.text = value.source || ""; rawTranslation = value.translation || ""
            displayedTranslation = value.styledSource ? rawTranslation.replace(/<\/?[bi]>/g, "") : rawTranslation
            translatedEditor.text = displayedTranslation
            regionX.text = value.region[0].toFixed(1); regionY.text = value.region[1].toFixed(1)
            regionW.text = value.region[2].toFixed(1); regionH.text = value.region[3].toFixed(1); pointSize.text = value.size.toFixed(1)
            alignCenter.checked = !!value.centered; boldText.checked = !!value.bold; serifText.checked = !!value.serif; keepOriginal.checked = !!value.keep
            confirmTranslation.checked = false; confirmNumbers.checked = !!value.numbersConfirmed
            backgroundMode.currentIndex = Math.max(0, ["auto", "repair", "panel", "original"].indexOf(value.backgroundMode || "auto"))
            textColorMode.currentIndex = Math.max(0, ["auto", "dark", "light"].indexOf(value.textColorMode || "auto"))
            panelColor.text = value.panelColor || "#ffffff"; confirmBackground.checked = !!value.backgroundConfirmed; open()
        }
        function save() {
            return Pdf.reviseBlock(block.id, { source: originalEditor.text, translation: translatedEditor.text === displayedTranslation ? rawTranslation : translatedEditor.text,
                region: [Number(regionX.text), Number(regionY.text), Number(regionW.text), Number(regionH.text)], size: Number(pointSize.text),
                centered: alignCenter.checked, bold: boldText.checked, serif: serifText.checked, keep: keepOriginal.checked, confirmTranslation: confirmTranslation.checked, confirmNumbers: confirmNumbers.checked,
                backgroundMode: ["auto", "repair", "panel", "original"][backgroundMode.currentIndex], textColorMode: ["auto", "dark", "light"][textColorMode.currentIndex], panelColor: panelColor.text, confirmBackground: confirmBackground.checked })
        }
        background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: editorDialog.height < 580 ? 7 : 12
            RowLayout {
                Layout.fillWidth: true
                UiText { text: "段落校对"; font.pixelSize: 19; font.weight: Font.DemiBold }
                Item { Layout.fillWidth: true }
                UiButton { objectName: "pdfShowRejectedResponse"; text: "查看失败返回"; compact: true; kind: "ghost"; visible: !!editorDialog.block.rejectedTranslation; onClicked: rejectedResponse.open() }
                UiButton { iconName: "close"; kind: "ghost"; compact: true; onClicked: editorDialog.close() }
            }
            UiText { visible: text.length > 0; text: Pdf.error || editorDialog.block.reviewReason || ""; color: Pdf.error ? Theme.danger : Theme.warning; wrapMode: Text.Wrap; Layout.fillWidth: true; maximumLineCount: 2; elide: Text.ElideRight }
            RowLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                    RowLayout {
                        Layout.fillWidth: true
                        UiText { text: "原文 · 可编辑"; color: Theme.secondary; font.pixelSize: 11; Layout.fillWidth: true }
                        UiButton { objectName: "pdfCopyBlockSource"; iconName: "copy"; compact: true; kind: "ghost"; hint: "复制此段原文"; onClicked: Pdf.copyText(originalEditor.text) }
                    }
                    UiScrollView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        TextArea { id: originalEditor; objectName: "pdfSourceEditor"; selectByMouse: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap; color: Theme.text; font.family: Theme.font; font.pixelSize: 14; padding: 12; onTextChanged: { confirmNumbers.checked = false; confirmBackground.checked = false } background: Rectangle { color: Theme.subtle; radius: 8 } }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                    RowLayout {
                        Layout.fillWidth: true
                        UiText { text: "译文 · 可编辑"; color: Theme.secondary; font.pixelSize: 11; Layout.fillWidth: true }
                        UiButton { iconName: "copy"; compact: true; kind: "ghost"; hint: "复制此段译文"; onClicked: Pdf.copyText(translatedEditor.text) }
                    }
                    UiScrollView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        TextArea { id: translatedEditor; objectName: "pdfTranslationEditor"; placeholderText: "输入或修改译文"; placeholderTextColor: Theme.muted; selectByMouse: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap; color: Theme.text; font.family: Theme.font; font.pixelSize: 14; padding: 12; onTextChanged: { confirmNumbers.checked = false; confirmBackground.checked = false } background: Rectangle { color: Theme.subtle; radius: 8 } }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                CheckBox { id: keepOriginal; onToggled: confirmBackground.checked = false; objectName: "pdfKeepOriginal"; text: "保留原文"; palette.windowText: Theme.text }
                CheckBox { id: alignCenter; onToggled: confirmBackground.checked = false; text: "居中"; palette.windowText: Theme.text }
                CheckBox { id: boldText; onToggled: confirmBackground.checked = false; text: "加粗"; palette.windowText: Theme.text }
                CheckBox { id: serifText; onToggled: confirmBackground.checked = false; text: "衬线字体"; palette.windowText: Theme.text }
                Item { Layout.fillWidth: true }
                UiText { text: "字号"; font.pixelSize: 11 }
                UiField { id: pointSize; onTextChanged: confirmBackground.checked = false; objectName: "pdfBlockFontSize"; Layout.preferredWidth: 64; implicitHeight: 32; validator: DoubleValidator { bottom: 7; top: 72; locale: "C" } }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                UiText { text: "排版区域（点）"; font.pixelSize: 11 }
                UiText { text: "X"; font.pixelSize: 11 }
                UiField { id: regionX; onTextChanged: confirmBackground.checked = false; objectName: "pdfBlockX"; Layout.fillWidth: true; Layout.preferredWidth: 60; implicitHeight: 32; validator: DoubleValidator { bottom: 0; locale: "C" } }
                UiText { text: "Y"; font.pixelSize: 11 }
                UiField { id: regionY; onTextChanged: confirmBackground.checked = false; objectName: "pdfBlockY"; Layout.fillWidth: true; Layout.preferredWidth: 60; implicitHeight: 32; validator: DoubleValidator { bottom: 0; locale: "C" } }
                UiText { text: "宽"; font.pixelSize: 11 }
                UiField { id: regionW; onTextChanged: confirmBackground.checked = false; objectName: "pdfBlockWidth"; Layout.fillWidth: true; Layout.preferredWidth: 60; implicitHeight: 32; validator: DoubleValidator { bottom: 3; locale: "C" } }
                UiText { text: "高"; font.pixelSize: 11 }
                UiField { id: regionH; onTextChanged: confirmBackground.checked = false; objectName: "pdfBlockHeight"; Layout.fillWidth: true; Layout.preferredWidth: 60; implicitHeight: 32; validator: DoubleValidator { bottom: 3; locale: "C" } }
            }
            CheckBox { id: confirmTranslation; objectName: "pdfConfirmBlockTranslation"; text: "原文有修改，已核对并确认当前译文"; visible: !!editorDialog.block.stale || originalEditor.text !== (editorDialog.block.source || ""); palette.windowText: Theme.text }
            RowLayout {
                Layout.fillWidth: true
                CheckBox { id: confirmNumbers; text: "已核对单元格数字"; visible: editorDialog.block.role === "cell"; palette.windowText: Theme.text }
                UiButton { text: "结构修改前的草稿"; compact: true; visible: !!editorDialog.block.draftTranslation || !!editorDialog.block.draftSource; onClicked: { oldDraft.customText = (editorDialog.block.draftSource || "") + "\n\n" + (editorDialog.block.draftTranslation || ""); oldDraft.open() } }
            }
            RowLayout {
                Layout.fillWidth: true; visible: !!editorDialog.block.scan; spacing: 6
                UiText { text: "背景"; font.pixelSize: 11 }
                UiCombo { id: backgroundMode; objectName: "pdfBackgroundMode"; model: ["自动清理纯色/渐变", "局部背景修复", "底色译文框", "保留原图对照"]; Layout.fillWidth: true; Layout.preferredWidth: 160; implicitHeight: 32; onActivated: confirmBackground.checked = false }
                UiCombo { id: textColorMode; objectName: "pdfTextColorMode"; model: ["自动字色", "深色文字", "浅色文字"]; Layout.preferredWidth: 112; implicitHeight: 32; onActivated: confirmBackground.checked = false }
                UiField { id: panelColor; objectName: "pdfPanelColor"; visible: backgroundMode.currentIndex === 2; Layout.preferredWidth: 100; implicitHeight: 32; placeholderText: "#ffffff"; onTextEdited: confirmBackground.checked = false }
            }
            RowLayout {
                visible: !!editorDialog.block.scan && backgroundMode.currentIndex === 1; Layout.fillWidth: true
                CheckBox { id: confirmBackground; objectName: "pdfConfirmBackground"; text: "已检查背景修复预览"; enabled: !!editorDialog.block.backgroundPreviewed && !Pdf.rendering; palette.windowText: Theme.text }
                UiText { Layout.fillWidth: true; text: "先保存并检查右页，再打开此区域确认；确认后可导出。"; wrapMode: Text.Wrap; font.pixelSize: 10; color: Theme.secondary }
            }
            UiText { visible: !!editorDialog.block.scan && backgroundMode.currentIndex === 3; Layout.fillWidth: true; text: "原图保留；译文仍可生成，在此窗口对照和复制，导出时该区域保持原图。"; wrapMode: Text.Wrap; color: Theme.secondary; font.pixelSize: 10 }
            UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "清空译文或选择「保留原文」可恢复该区域。调整区域时请避开图片、线条和相邻段落。"; color: Theme.secondary; font.pixelSize: 11 }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { objectName: "pdfRetryBlockButton"; text: "重新翻译此段"; compact: true; enabled: page.canTranslate && !Pdf.busy && !Pdf.processing && !keepOriginal.checked && !editorDialog.block.fixed; onClicked: if (editorDialog.save() && Pdf.retryBlock(editorDialog.block.id)) editorDialog.close() }
                UiButton { objectName: "pdfApplyEditButton"; text: "保存并排版"; kind: "primary"; compact: true; enabled: !Pdf.busy && !Pdf.processing; onClicked: if (editorDialog.save()) editorDialog.close() }
            }
        }
    }

    Dialog {
        id: rejectedResponse; objectName: "pdfRejectedResponseDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(680, parent.width - 40); height: Math.min(480, parent.height - 40)
        modal: true; padding: 20
        background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 12
            UiText { text: "最近一次失败返回"; font.pixelSize: 19; font.weight: Font.DemiBold }
            UiText { text: "以下内容未写入译文，供核对服务返回。"; color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
            UiText { text: editorDialog.block.translationError || ""; color: Theme.warning; wrapMode: Text.Wrap; Layout.fillWidth: true }
            UiScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                TextArea { objectName: "pdfRejectedResponseText"; text: editorDialog.block.rejectedTranslation || ""; readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap; color: Theme.text; font.family: Theme.font; font.pixelSize: 14; padding: 12; background: Rectangle { color: Theme.subtle; radius: 8 } }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { objectName: "pdfCopyRejectedResponse"; text: "复制返回"; compact: true; onClicked: Pdf.copyText(editorDialog.block.rejectedTranslation || "") }
                UiButton { objectName: "pdfRejectedResponseClose"; text: "关闭"; compact: true; onClicked: rejectedResponse.close() }
            }
        }
    }

    PdfSettingsDialog { id: settingsDialog; onManageProviders: page.openSettings() }
    PdfIssuesDialog { id: issuesDialog; onLocateBlock: function(id) { page.focusBlock(id) } }

    RowLayout {
        objectName: "pdfToolbar"; Layout.fillWidth: true; spacing: 6
        UiText { Layout.fillWidth: true; Layout.minimumWidth: 60; text: Pdf.fileName || "PDF 翻译"; elide: Text.ElideMiddle; font.pixelSize: 16; font.weight: Font.DemiBold; textFormat: Text.PlainText }
        UiText { objectName: "pdfActualModel"; Layout.maximumWidth: 130; text: (Pdf.recognizing ? "识图 · " : "模型 · ") + (Pdf.taskModel || "未选择"); elide: Text.ElideRight; color: Theme.secondary; font.pixelSize: 10; textFormat: Text.PlainText }
        UiButton { objectName: "pdfSettingsButton"; text: "翻译设置"; compact: true; kind: "ghost"; onClicked: settingsDialog.open() }
        UiButton { objectName: "pdfImportButton"; text: "打开"; hint: "打开或拖入 PDF 文件"; compact: true; enabled: !Pdf.busy && !Pdf.processing; onClicked: openFile.open() }
        UiButton {
            id: moreButton; objectName: "pdfMoreButton"; text: "更多"; compact: true; onClicked: moreMenu.open()
            Menu {
                id: moreMenu; objectName: "pdfMoreMenu"; y: moreButton.height + 4; width: 220
                palette.text: Theme.text; palette.window: Theme.surface; palette.highlight: Theme.accentSoft; palette.highlightedText: Theme.text
                background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
                MenuItem { objectName: "pdfReadingMode"; text: page.expanded ? "显示侧栏" : "展开阅读"; onTriggered: page.toggleReading() }
                MenuSeparator {}
                MenuItem { objectName: "pdfRetranslatePage"; text: "重译当前页"; enabled: page.canTranslate && Pdf.pageTotal > 0 && !Pdf.busy && !Pdf.processing && !Pdf.taskPending; onTriggered: Pdf.retranslatePage() }
                MenuItem { objectName: "pdfEditFirstBlock"; text: "校对段落"; enabled: Pdf.blocks.length > 0 && !Pdf.busy && !Pdf.processing; onTriggered: { var items = Pdf.blocks; for (var i = 0; i < items.length; ++i) if (!items[i].fixed) { page.focusBlock(items[i].id); break } } }
                MenuItem { objectName: "pdfStructureButton"; text: "结构校正"; enabled: Pdf.blocks.length > 0 && !Pdf.busy && !Pdf.processing; onTriggered: structureDialog.open() }
                MenuItem { objectName: "pdfTextView"; text: page.textView ? "版式对照" : "整页文字"; enabled: Pdf.scanMode; onTriggered: page.textView = !page.textView }
                MenuSeparator {}
                MenuItem { objectName: "pdfRecognizeCurrent"; text: "仅识别当前页"; enabled: Pdf.loaded && !Pdf.busy && !Pdf.processing && page.canTranslate; onTriggered: page.recognizeCurrent() }
                MenuItem { objectName: "pdfRecognizeRange"; text: "批量识别…"; enabled: Pdf.loaded && !Pdf.busy && !Pdf.processing && page.canTranslate; onTriggered: recognitionRange.open() }
                MenuItem { objectName: "pdfCropButton"; text: "框选取字…"; enabled: !Pdf.busy && !Pdf.processing && page.canTranslate && Pdf.originalPreview.toString().length > 0; onTriggered: cropDialog.open() }
                MenuSeparator {}
                MenuItem { objectName: "pdfAnalysisButton"; text: Pdf.resetPending ? "继续清除" : Pdf.analyzing ? "暂停解析" : "继续解析"; enabled: (Pdf.resetPending && !Pdf.resetting) || (Pdf.pageCount > 0 && !Pdf.analysisComplete && !Pdf.processing && !Pdf.busy); onTriggered: Pdf.resetPending || !Pdf.analyzing ? Pdf.resumeAnalysis() : Pdf.pauseAnalysis() }
                MenuItem { objectName: "pdfDiscardTask"; text: "结束暂停的任务"; enabled: Pdf.taskPending && !Pdf.busy && !Pdf.processing; onTriggered: Pdf.discardTask() }
                MenuItem { objectName: "pdfResetButton"; text: "清空全书译文…"; enabled: Pdf.loaded && !Pdf.busy && !Pdf.processing && (Pdf.taskDescription.length > 0 || Pdf.completed > 0); onTriggered: resetNotice.open() }
            }
        }
        UiButton {
            objectName: "pdfExportButton"; text: "导出"; compact: true; kind: "soft"
            enabled: Pdf.completed > 0 && !Pdf.busy && !Pdf.processing
            onClicked: { if (!Pdf.analysisComplete || Pdf.completed < Pdf.total || Pdf.unsupportedPages > 0) partialNotice.open(); else page.chooseOutput() }
        }
    }
    RowLayout {
        objectName: "pdfControls"; Layout.fillWidth: true; spacing: 5
        UiButton { objectName: "pdfPreviousPage"; text: "‹"; compact: true; kind: "ghost"; hint: "上一页"; enabled: Pdf.loaded && Pdf.page > 0; onClicked: Pdf.page-- }
        UiField {
            id: pageNumber; objectName: "pdfPageNumber"; Layout.preferredWidth: 48; implicitHeight: 34
            text: String(Pdf.page + 1); horizontalAlignment: TextInput.AlignHCenter; leftPadding: 3; rightPadding: 3
            enabled: Pdf.loaded; validator: IntValidator { bottom: 1; top: Math.max(1, Pdf.pageCount) }
            Accessible.name: "跳转页码"
            onEditingFinished: { var p = Number(text); if (acceptableInput) Pdf.page = p - 1; text = String(Pdf.page + 1) }
        }
        UiText { text: "/ " + Pdf.pageCount; color: Theme.secondary; font.pixelSize: 12 }
        UiButton { objectName: "pdfNextPage"; text: "›"; compact: true; kind: "ghost"; hint: "下一页"; enabled: Pdf.loaded && Pdf.page + 1 < Pdf.pageCount; onClicked: Pdf.page++ }
        UiCombo {
            objectName: "pdfZoom"; model: ["适合双页", "放大 125%", "放大 150%", "放大 200%"]
            Layout.preferredWidth: 104; implicitHeight: 34; enabled: Pdf.loaded; quiet: true
            onActivated: { page.zoom = [1, 1.25, 1.5, 2][currentIndex]; viewer.contentX = 0; viewer.contentY = 0 }
        }
        Item { Layout.fillWidth: true }
        UiCombo {
            objectName: "pdfTarget"; Layout.preferredWidth: 104; implicitHeight: 34; quiet: true
            model: ["简体中文", "繁體中文", "英语"]; currentIndex: model.indexOf(Pdf.taskTarget)
            enabled: !Pdf.busy && !Pdf.processing && !Pdf.taskProviderId.length
            onActivated: App.targetLanguage = currentText
        }
        UiCombo {
            id: scope; objectName: "pdfScope"; model: ["整篇文档", "当前页"]; currentIndex: 1
            displayText: Pdf.taskPending ? Pdf.taskScope : currentText
            Layout.preferredWidth: 106; implicitHeight: 34; quiet: true; enabled: !Pdf.busy && !Pdf.processing && !Pdf.taskPending
        }
        UiButton {
            objectName: "pdfTranslateButton"; kind: enabled ? "primary" : "soft"; compact: true
            text: Pdf.busy ? "暂停" : Pdf.processing ? (Pdf.resetPending && !Pdf.resetting ? "继续清除" : "取消处理")
                : Pdf.taskPending ? "继续" : page.needsRecognition ? "识别并翻译"
                : !page.scopeReady ? "翻译" : page.scopeTotal === 0 ? "无可译内容"
                : page.scopeCompleted === page.scopeTotal ? "已完成" : "翻译"
            enabled: Pdf.busy || Pdf.processing || (Pdf.loaded && (Pdf.taskPending || page.needsRecognition || !page.scopeReady || page.scopeCompleted < page.scopeTotal) && page.canTranslate)
            onClicked: {
                if (Pdf.resetPending && !Pdf.resetting) Pdf.resumeAnalysis()
                else if (Pdf.busy || Pdf.processing) Pdf.cancel()
                else Pdf.translate(scope.currentIndex === 1, page.recognitionPixels)
            }
        }
    }
    Rectangle {
        visible: Pdf.error.length > 0; Layout.fillWidth: true; implicitHeight: errorText.implicitHeight + 16; radius: 8; color: Theme.dangerSoft
        UiText { id: errorText; objectName: "pdfError"; anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 } text: Pdf.error; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: 11; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight }
    }
    Rectangle {
        id: preview; objectName: "pdfPreviewArea"
        Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 100
        color: Theme.subtle; radius: 10; border.color: Theme.border
        Row {
            visible: Pdf.loaded && (!Pdf.scanMode || !page.textView)
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
            spacing: 12
            UiText { objectName: "pdfOriginalPageStatus"; width: (parent.width - 12) / 2; text: "原文"; color: Theme.secondary; font.pixelSize: 10 }
            UiText {
                objectName: "pdfTranslatedPageStatus"; width: (parent.width - 12) / 2
                text: "译文 · " + Pdf.pageStatus; color: Pdf.pageIssues.length ? Theme.warning : Theme.secondary
                font.pixelSize: 10; elide: Text.ElideRight
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: issuesDialog.open() }
            }
        }
        Flickable {
            id: viewer; objectName: "pdfViewer"; anchors.fill: parent; anchors.margins: 8; anchors.topMargin: 28; clip: true
            visible: Pdf.loaded && (!Pdf.scanMode || !page.textView); boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
            contentWidth: Math.max(width, pagesRow.width); contentHeight: pagesRow.height
            flickableDirection: Flickable.HorizontalAndVerticalFlick
            ScrollBar.vertical: UiScrollBar {}
            ScrollBar.horizontal: UiScrollBar {}
            WheelHandler {
                target: null
                onWheel: function(event) {
                    var horizontal = (event.modifiers & Qt.ShiftModifier) !== 0
                    var amount = event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 120 * 60
                    viewer.cancelFlick()
                    if (horizontal) viewer.contentX = Math.max(0, Math.min(Math.max(0, viewer.contentWidth - viewer.width), viewer.contentX - amount))
                    else viewer.contentY = Math.max(0, Math.min(Math.max(0, viewer.contentHeight - viewer.height), viewer.contentY - amount))
                    event.accepted = true
                }
            }
            Row {
                id: pagesRow; spacing: 12
                Repeater {
                    model: [false, true]
                    delegate: Column {
                        id: sheet; required property bool modelData
                        width: Math.max(120, (viewer.width - 12) / 2) * page.zoom; spacing: 6
                        Rectangle {
                            width: sheet.width; height: width / Pdf.pageRatio; color: "white"
                            Image { objectName: sheet.modelData ? "pdfTranslatedPageImage" : "pdfOriginalPageImage"; anchors.fill: parent; source: sheet.modelData ? Pdf.translatedPreview : Pdf.originalPreview; fillMode: Image.PreserveAspectFit; cache: false; asynchronous: true }
                            Repeater {
                                model: sheet.modelData ? Pdf.blocks : []
                                delegate: Rectangle {
                                    required property var modelData
                                    x: modelData.region[0] / Pdf.pageWidth * sheet.width
                                    y: modelData.region[1] / Pdf.pageWidth * sheet.width
                                    width: modelData.region[2] / Pdf.pageWidth * sheet.width
                                    height: modelData.region[3] / Pdf.pageWidth * sheet.width
                                    visible: !modelData.fixed || !!modelData.scan; color: hover.hovered ? "#18347457" : "transparent"
                                    border.width: modelData.reviewReason || hover.hovered || page.focusedBlockId === modelData.id ? 1 : 0
                                    border.color: modelData.reviewReason ? Theme.warning : Theme.accent
                                    AbstractButton {
                                        id: hover; objectName: "pdfBlock_" + parent.modelData.id
                                        anchors.fill: parent; hoverEnabled: true
                                        enabled: !Pdf.busy && !Pdf.processing
                                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                                        onClicked: editorDialog.edit(parent.modelData)
                                    }
                                    UiToolTip { targetControl: hover; text: hover.parent.modelData.reviewReason || hover.parent.modelData.preserveReason || (hover.parent.modelData.backgroundReview && !hover.parent.modelData.backgroundConfirmed ? "检查背景修复效果并确认" : "点击校对这一段") }
                                }
                            }
                        }
                    }
                }
            }
        }
        RowLayout {
            anchors.fill: parent; anchors.margins: 8; spacing: 12; visible: Pdf.loaded && Pdf.scanMode && page.textView
            Flickable {
                id: scanImageView; objectName: "pdfScanImageView"; Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                contentWidth: scanPaper.width; contentHeight: scanPaper.height; clip: true
                boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
                ScrollBar.vertical: UiScrollBar {}
                ScrollBar.horizontal: UiScrollBar {}
                Rectangle {
                    id: scanPaper; width: scanImageView.width * page.zoom; height: width / Pdf.pageRatio; color: "white"
                    Image { anchors.fill: parent; source: Pdf.originalPreview; fillMode: Image.PreserveAspectFit; cache: false; asynchronous: true }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1; spacing: 5
                RowLayout {
                    Layout.fillWidth: true; spacing: 4
                    UiButton { objectName: "pdfScanSourceTab"; text: "识别原文"; compact: true; kind: page.scanTab === 0 ? "soft" : "ghost"; onClicked: page.scanTab = 0 }
                    UiButton { objectName: "pdfScanTranslationTab"; text: "译文"; compact: true; kind: page.scanTab === 1 ? "soft" : "ghost"; onClicked: page.scanTab = 1 }
                    UiButton { text: "旧稿"; compact: true; visible: Pdf.recognitionDraft.length > 0; onClicked: { oldDraft.customText = ""; oldDraft.open() } }
                    Item { Layout.fillWidth: true }
                    UiButton { objectName: "pdfCopyScanText"; iconName: "copy"; hint: page.scanTab === 0 ? "复制原文" : "复制译文"; compact: true; enabled: scanEditor.text.length > 0; onClicked: Pdf.copyText(scanEditor.text) }
                }
                UiText { objectName: "pdfScanPageStatus"; Layout.fillWidth: true; text: Pdf.pageStatus; wrapMode: Text.Wrap; font.pixelSize: 10; color: Pdf.translationStale ? Theme.danger : Theme.secondary }
                UiScrollView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    TextArea {
                        id: scanEditor; objectName: "pdfScanEditor"; textFormat: TextEdit.PlainText; selectByMouse: true; wrapMode: TextEdit.Wrap
                        readOnly: Pdf.layoutReady || Pdf.busy || Pdf.processing; font.family: Theme.font; font.pixelSize: 14; color: Theme.text; padding: 10
                        placeholderText: page.scanTab === 0 ? "点击「识别并翻译」，或在「更多」中仅识别原文。" : "点击「翻译」生成本页译文。"
                        placeholderTextColor: Theme.muted; background: Rectangle { color: Theme.surface; radius: 8 }
                        onTextChanged: if (!page.syncingScan && Pdf.scanMode && !Pdf.busy && !Pdf.processing) { if (page.scanTab === 0) Pdf.scanSource = text; else Pdf.scanTranslation = text }
                    }
                }
                UiText {
                    Layout.fillWidth: true; visible: !Pdf.layoutReady && Pdf.scanSource.length > 0
                    text: "旧版草稿缺少段落坐标，请重新识别后排版。草稿会保留供复制。"
                    wrapMode: Text.Wrap; font.pixelSize: 11; color: Theme.secondary
                }
            }
        }
        ColumnLayout {
            anchors.centerIn: parent; visible: !Pdf.loaded; spacing: 14
            Icon { name: "document"; Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 42; Layout.preferredHeight: 42; color: Theme.accent }
            UiText { text: "在原版式中，读懂另一种语言"; Layout.alignment: Qt.AlignHCenter; font.pixelSize: 17 }
            UiText { text: "文字页直接翻译 · 扫描页自动识别后翻译\n保留版式，随时校对"; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter; color: Theme.secondary; font.pixelSize: 12; lineHeight: 1.6 }
        }
        BusyIndicator { anchors.centerIn: parent; visible: Pdf.processing || Pdf.rendering; running: visible; palette.dark: Theme.accent }
    }
    ProgressBar {
        objectName: "pdfActivityProgress"; Layout.fillWidth: true; visible: Pdf.busy || Pdf.processing || Pdf.analyzing
        from: 0; to: 1; value: Pdf.activityProgress
        background: Rectangle { implicitHeight: 3; radius: 2; color: Theme.border }
        contentItem: Item { implicitHeight: 3; Rectangle { width: parent.width * parent.parent.visualPosition; height: parent.height; radius: 2; color: Theme.accent } }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 8
        UiText {
            objectName: "pdfStatus"; Layout.fillWidth: true; text: Pdf.activityStatus; font.pixelSize: 11
            color: !Pdf.busy && Pdf.pageIssues.length ? Theme.warning : Theme.secondary; elide: Text.ElideRight
            MouseArea { anchors.fill: parent; enabled: Pdf.loaded; cursorShape: Qt.PointingHandCursor; onClicked: issuesDialog.open() }
        }
        UiButton {
            objectName: "pdfIssuesButton"; compact: true; kind: "ghost"; enabled: Pdf.loaded
            text: Pdf.pendingPages || Pdf.reviewPages ? (Pdf.pendingPages ? Pdf.pendingPages + " 页待处理" : "") + (Pdf.pendingPages && Pdf.reviewPages ? " · " : "") + (Pdf.reviewPages ? Pdf.reviewPages + " 页待校对" : "") : "文档进度"
            onClicked: issuesDialog.open()
        }
        UiText { text: Pdf.loaded ? (Pdf.busy ? "自动保存" : Pdf.error.length ? "请查看提示" : "已保存") : ""; color: Theme.muted; font.pixelSize: 10 }
    }
    Connections {
        target: Pdf
        function onRegionRecognized() { regionEditor.text = Pdf.regionText; regionResult.open() }
        function onChanged() {
            if (page.lastPage !== Pdf.page) { viewer.contentX = 0; viewer.contentY = 0; page.focusedBlockId = ""; pageNumber.text = String(Pdf.page + 1) }
            if (page.lastPage !== Pdf.page || (page.wasRecognizing && !Pdf.recognizing)) { page.scanTab = 0; page.lastPage = Pdf.page; scanImageView.contentY = 0 }
            page.wasRecognizing = Pdf.recognizing
            page.syncScan()
            if (viewer.contentY > Math.max(0, viewer.contentHeight - viewer.height)) viewer.contentY = 0
        }
    }
}
