import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog; objectName: "pdfStructureDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(1080, parent.width - 40); height: parent.height - 40; modal: true; padding: 16
    signal editBlockRequested(var block)
    property var selected: ({})
    property var selectedIds: []
    property var draft: [0, 0, 0, 0]
    readonly property var items: {
        if (mode.currentIndex === 0) return Pdf.blocks
        var result = [], all = Pdf.layoutRegions
        for (var i = 0; i < all.length; ++i)
            if ((mode.currentIndex === 2) === (all[i].kind === "table")) result.push(all[i])
        return result
    }
    function select(item) {
        selected = item
        var r = mode.currentIndex === 0 ? item.region : item.rect
        setDraft([r[0], r[1], r[2], r[3]])
        order.value = (item.order || 0) + 1; headers.value = item.headerRows || 0
        previousPage.value = item.continuedFrom ? item.continuedFrom.page + 1 : 0
        if (mode.currentIndex === 0 && multiple.checked) {
            var ids = selectedIds.slice(), at = ids.indexOf(item.id)
            if (at >= 0) ids.splice(at, 1); else ids.push(item.id)
            selectedIds = ids
        } else selectedIds = [item.id]
    }
    function setDraft(r) {
        draft = r; xField.text = r[0].toFixed(1); yField.text = r[1].toFixed(1)
        widthField.text = r[2].toFixed(1); heightField.text = r[3].toFixed(1)
    }
    function resetSelection() { selected = ({}); selectedIds = [] }
    onOpened: { resetSelection(); paperView.contentY = 0 }
    background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            UiText { text: "页面结构校正"; font.pixelSize: 18; font.weight: Font.DemiBold; Layout.fillWidth: true }
            UiCombo { id: mode; objectName: "pdfStructureMode"; model: ["文字区域", "栏与单元格", "表格设置"]; Layout.preferredWidth: 140; implicitHeight: 32; onActivated: dialog.resetSelection() }
            CheckBox { id: multiple; objectName: "pdfStructureMultiple"; text: "多选"; visible: mode.currentIndex === 0; palette.windowText: Theme.text; onToggled: dialog.selectedIds = [] }
        }
        UiText { Layout.fillWidth: true; text: Pdf.error || "点击区域进行校正；拖动选中区域的角点预览边界，点击应用保存。"; color: Pdf.error ? Theme.danger : Theme.secondary; font.pixelSize: 11; wrapMode: Text.Wrap; maximumLineCount: 2 }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12
            Flickable {
                id: paperView; objectName: "pdfStructureViewer"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                contentWidth: width; contentHeight: paper.height; boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
                ScrollBar.vertical: UiScrollBar {}
                Rectangle {
                    id: paper; width: paperView.width; height: width / Pdf.pageRatio; color: "white"
                    readonly property real factor: width / Pdf.pageWidth
                    Image { anchors.fill: parent; source: Pdf.originalPreview; fillMode: Image.PreserveAspectFit; cache: false }
                    Repeater {
                        model: dialog.items
                        delegate: Rectangle {
                            id: region; required property var modelData
                            readonly property bool chosen: dialog.selected.id === modelData.id
                            readonly property var box: chosen ? dialog.draft : mode.currentIndex === 0 ? modelData.region : modelData.rect
                            x: box[0] * paper.factor; y: box[1] * paper.factor; width: box[2] * paper.factor; height: box[3] * paper.factor
                            color: dialog.selectedIds.indexOf(modelData.id) >= 0 ? "#34347457" : "#08347457"
                            border.color: chosen ? "#d28a2f" : "#347457"; border.width: chosen ? 2 : 1
                            z: chosen ? 3 : modelData.kind === "cell" ? 2 : 1
                            MouseArea { objectName: "pdfStructureRegion_" + region.modelData.id; anchors.fill: parent; onClicked: dialog.select(region.modelData); cursorShape: Qt.PointingHandCursor }
                            UiText { text: mode.currentIndex === 0 ? String((region.modelData.order || 0) + 1) : region.modelData.kind === "cell" ? (region.modelData.row + 1) + "," + (region.modelData.column + 1) : region.modelData.kind === "table" ? "表格" : "栏"; font.pixelSize: 10; color: "#225638" }
                            Repeater {
                                model: region.chosen && mode.currentIndex !== 2 ? 4 : 0
                                delegate: Rectangle {
                                    id: handle; required property int index
                                    width: 12; height: 12; radius: 6; color: "#d28a2f"; border.color: "white"
                                    x: (index % 2 ? region.width : 0) - 6; y: (index >= 2 ? region.height : 0) - 6
                                    MouseArea {
                                        objectName: "pdfStructureHandle_" + region.modelData.id + "_" + handle.index
                                        anchors.fill: parent; cursorShape: Qt.SizeFDiagCursor; property var startRect
                                        onPressed: startRect = dialog.draft.slice()
                                        onPositionChanged: function(mouse) {
                                            if (!pressed) return
                                            var p = mapToItem(paper, mouse.x, mouse.y), r = startRect.slice()
                                            var px = Math.max(0, Math.min(Pdf.pageWidth, p.x / paper.factor))
                                            var py = Math.max(0, Math.min(Pdf.pageWidth / Pdf.pageRatio, p.y / paper.factor))
                                            if (handle.index % 2) r[2] = Math.max(3, px - r[0]); else { r[0] = Math.min(px, r[0] + r[2] - 3); r[2] = startRect[0] + startRect[2] - r[0] }
                                            if (handle.index >= 2) r[3] = Math.max(3, py - r[1]); else { r[1] = Math.min(py, r[1] + r[3] - 3); r[3] = startRect[1] + startRect[3] - r[1] }
                                            dialog.setDraft(r)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            UiScrollView {
                Layout.preferredWidth: 250; Layout.fillHeight: true; clip: true
                ColumnLayout {
                    width: parent.width; spacing: 10
                    UiText { Layout.fillWidth: true; text: dialog.selected.id ? mode.currentIndex === 0 ? "文字区域 " + ((dialog.selected.order || 0) + 1) : dialog.selected.kind === "table" ? "表格 · " + dialog.selected.rows + " 行 × " + dialog.selected.columns + " 列" : dialog.selected.kind === "cell" ? "单元格" : "栏边界" : "选择左侧区域"; font.pixelSize: 14; font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                    UiText { Layout.fillWidth: true; text: dialog.selected.source || ""; font.pixelSize: 11; wrapMode: Text.Wrap; maximumLineCount: 5; elide: Text.ElideRight; visible: mode.currentIndex === 0 }
                    GridLayout {
                        columns: 2; visible: !!dialog.selected.id && mode.currentIndex !== 2; Layout.fillWidth: true
                        UiText { text: "X / 点"; font.pixelSize: 11 } UiText { text: "Y / 点"; font.pixelSize: 11 }
                        UiField { id: xField; objectName: "pdfStructureX"; Layout.fillWidth: true; Layout.preferredWidth: 100; implicitHeight: 32 }
                        UiField { id: yField; objectName: "pdfStructureY"; Layout.fillWidth: true; Layout.preferredWidth: 100; implicitHeight: 32 }
                        UiText { text: "宽 / 点"; font.pixelSize: 11 } UiText { text: "高 / 点"; font.pixelSize: 11 }
                        UiField { id: widthField; objectName: "pdfStructureWidth"; Layout.fillWidth: true; Layout.preferredWidth: 100; implicitHeight: 32 }
                        UiField { id: heightField; objectName: "pdfStructureHeight"; Layout.fillWidth: true; Layout.preferredWidth: 100; implicitHeight: 32 }
                    }
                    UiButton {
                        objectName: "pdfStructureApply"; text: "应用边界"; compact: true; visible: mode.currentIndex !== 2; enabled: !!dialog.selected.id
                        onClicked: {
                            var r = [Number(xField.text), Number(yField.text), Number(widthField.text), Number(heightField.text)]
                            var ok = mode.currentIndex === 0 ? Pdf.reviseBlock(dialog.selected.id, {region:r}) : Pdf.reviseRegion(dialog.selected.id, {rect:r})
                            if (ok) dialog.resetSelection()
                        }
                    }
                    UiText { Layout.fillWidth: true; visible: !!dialog.selected.preserveReason; text: dialog.selected.preserveReason || ""; wrapMode: Text.Wrap; font.pixelSize: 11; color: Theme.secondary }
                    ColumnLayout {
                        visible: mode.currentIndex === 0 && !!dialog.selected.id; Layout.fillWidth: true
                        UiButton { text: "编辑原文与译文"; compact: true; onClicked: { var b = dialog.selected; dialog.close(); dialog.editBlockRequested(b) } }
                        RowLayout { UiText { text: "阅读顺序"; font.pixelSize: 11 } SpinBox { id: order; objectName: "pdfStructureOrder"; from: 1; to: Math.max(1, Pdf.blocks.length); editable: true; Layout.fillWidth: true } }
                        UiButton { objectName: "pdfStructureMove"; text: "调整顺序"; compact: true; onClicked: { if (Pdf.moveBlock(dialog.selected.id, order.value - 1)) dialog.resetSelection() } }
                        UiButton { objectName: "pdfStructureSplit"; text: "拆分区域"; compact: true; onClicked: { splitSourceA.text = dialog.selected.source || ""; splitSourceB.text = ""; splitDialog.open() } }
                        UiButton { objectName: "pdfStructureMerge"; text: "合并选中区域（" + dialog.selectedIds.length + "）"; compact: true; enabled: dialog.selectedIds.length > 1; onClicked: { if (Pdf.mergeBlocks(dialog.selectedIds)) dialog.resetSelection() } }
                        UiText { Layout.fillWidth: true; text: "拆分或合并后重新翻译。原译文保留为草稿，可撤销结构修改。"; wrapMode: Text.Wrap; font.pixelSize: 11; color: Theme.secondary }
                    }
                    ColumnLayout {
                        visible: mode.currentIndex === 2 && !!dialog.selected.id; Layout.fillWidth: true
                        UiText { Layout.fillWidth: true; text: dialog.selected.inferred ? "无边框推断 · 请检查行列与表头" : "已检测表格网格"; font.pixelSize: 11; wrapMode: Text.Wrap }
                        RowLayout { UiText { text: "表头行数"; font.pixelSize: 11 } SpinBox { id: headers; objectName: "pdfTableHeaders"; from: 0; to: dialog.selected.rows || 1; editable: true; Layout.fillWidth: true } }
                        UiButton { objectName: "pdfTableApplyHeaders"; text: "应用表头"; compact: true; onClicked: { if (Pdf.reviseRegion(dialog.selected.id, {headerRows:headers.value})) dialog.resetSelection() } }
                        UiText { Layout.fillWidth: true; text: "沿用前页表头（0 为解除关联）"; wrapMode: Text.Wrap; font.pixelSize: 11 }
                        SpinBox { id: previousPage; from: 0; to: Pdf.page; editable: true; Layout.fillWidth: true }
                        UiButton { text: "关联表头"; compact: true; onClicked: { if (Pdf.reviseRegion(dialog.selected.id, {previousPage:previousPage.value - 1})) dialog.resetSelection() } }
                        UiText { Layout.fillWidth: true; text: "关联后用前页表头帮助理解单元格，页面与表格位置保持不变。"; wrapMode: Text.Wrap; font.pixelSize: 11; color: Theme.secondary }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            UiButton { objectName: "pdfStructureUndo"; text: "撤销结构修改"; compact: true; enabled: Pdf.canUndoLayout; onClicked: { if (Pdf.undoLayout()) dialog.resetSelection() } }
            Item { Layout.fillWidth: true }
            UiButton { objectName: "pdfStructureClose"; text: "完成"; compact: true; kind: "primary"; onClicked: dialog.close() }
        }
    }
    Dialog {
        id: splitDialog; objectName: "pdfSplitDialog"; parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(600, parent.width - 40); height: Math.min(500, parent.height - 40); padding: 20; modal: true
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        function resetPosition() { var r = dialog.selected.role === "cell" ? dialog.selected.containerRect : dialog.selected.region; if (r) splitPosition.text = (splitDirection.currentIndex === 1 ? r[0] + r[2] / 2 : r[1] + r[3] / 2).toFixed(1) }
        onOpened: resetPosition()
        contentItem: ColumnLayout {
            spacing: 10
            UiText { text: "拆分并校对原文"; font.pixelSize: 18 }
            RowLayout {
                UiCombo { id: splitDirection; objectName: "pdfSplitDirection"; model: ["上下拆分", "左右拆分"]; Layout.preferredWidth: 120; onActivated: splitDialog.resetPosition() }
                UiText { text: "分界坐标 / 点"; font.pixelSize: 11 }
                UiField { id: splitPosition; objectName: "pdfSplitPosition"; Layout.fillWidth: true }
            }
            UiText { Layout.fillWidth: true; text: "分别填写上/左与下/右区域的原文。共享同一原文字对象的区域会在全部翻译完成后一起替换。"; wrapMode: Text.Wrap; font.pixelSize: 11; color: Theme.secondary }
            RowLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                UiScrollView { Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1; clip: true
                    TextArea { id: splitSourceA; objectName: "pdfSplitSourceA"; color: Theme.text; wrapMode: TextEdit.Wrap; selectByMouse: true; textFormat: TextEdit.PlainText; background: Rectangle { color: Theme.subtle } }
                }
                UiScrollView { Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1; clip: true
                    TextArea { id: splitSourceB; objectName: "pdfSplitSourceB"; color: Theme.text; wrapMode: TextEdit.Wrap; selectByMouse: true; textFormat: TextEdit.PlainText; background: Rectangle { color: Theme.subtle } }
                }
            }
            UiText { Layout.fillWidth: true; text: Pdf.error; visible: text.length > 0; font.pixelSize: 11; color: Theme.danger; wrapMode: Text.Wrap }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UiButton { text: "取消"; compact: true; onClicked: splitDialog.close() }
                UiButton { objectName: "pdfConfirmSplit"; text: "应用拆分"; compact: true; kind: "primary"; onClicked: {
                    if (Pdf.splitBlock(dialog.selected.id, splitDirection.currentIndex === 1, Number(splitPosition.text), splitSourceA.text, splitSourceB.text)) { splitDialog.close(); dialog.resetSelection() }
                } }
            }
        }
    }
}
