import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog; objectName: "pdfSettingsDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(480, parent.width - 40); modal: true; padding: 20
    signal manageProviders()
    readonly property int recognitionPixels: quality.currentIndex === 1 ? 2400 : 1800
    background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 14
        UiText { text: "翻译设置"; font.pixelSize: 18; font.weight: Font.DemiBold }
        UiText { text: "翻译模型"; color: Theme.secondary; font.pixelSize: 12 }
        UiCombo {
            objectName: "pdfProvider"; Layout.fillWidth: true
            model: App.providers; textRole: "name"
            currentIndex: { var id = Pdf.taskProviderId || App.selectedProviderId; for (var i = 0; i < model.length; ++i) if (model[i].id === id) return i; return -1 }
            displayText: currentIndex >= 0 ? currentText : "选择翻译模型"
            enabled: !Pdf.busy && !Pdf.processing && !Pdf.taskProviderId.length
            onActivated: App.selectProvider(model[currentIndex].id)
        }
        UiText {
            objectName: "pdfTaskModelDetails"; Layout.fillWidth: true; wrapMode: Text.Wrap
            text: Pdf.taskDescription.length ? "本任务使用 " + Pdf.taskDescription + "。继续和重译沿用此设置；清空全书译文后可更换。" : "模型：" + (App.modelName || "尚未选择")
            color: Theme.secondary; font.pixelSize: 11; textFormat: Text.PlainText
        }
        UiText { text: "扫描页识图模型"; color: Theme.secondary; font.pixelSize: 12 }
        UiCombo {
            objectName: "pdfVisionProvider"; Layout.fillWidth: true
            model: App.providers; textRole: "name"; enabled: !Pdf.busy && !Pdf.processing && !Pdf.taskPending
            currentIndex: { for (var i = 0; i < model.length; ++i) if (model[i].id === Pdf.visionProviderId) return i; return -1 }
            displayText: currentIndex >= 0 ? currentText : "选择支持图片的模型"
            onActivated: Pdf.visionProviderId = model[currentIndex].id
        }
        RowLayout {
            UiText { text: "识别清晰度"; color: Theme.secondary; font.pixelSize: 12 }
            UiCombo { id: quality; objectName: "pdfRecognitionQuality"; Layout.fillWidth: true; model: ["标准", "高清"]; enabled: !Pdf.busy && !Pdf.taskPending }
        }
        UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "扫描页会先识别，再自动翻译。高清适合小字，处理时间更长。"; color: Theme.secondary; font.pixelSize: 11 }
        RowLayout {
            UiText { text: "无响应等待"; color: Theme.secondary; font.pixelSize: 12 }
            UiCombo {
                objectName: "pdfRequestTimeout"; Layout.fillWidth: true; model: ["5 分钟", "10 分钟", "20 分钟", "30 分钟", "60 分钟"]
                readonly property var minutes: [5, 10, 20, 30, 60]
                currentIndex: minutes.indexOf(Pdf.requestTimeoutMinutes)
                displayText: Pdf.requestTimeoutMinutes + " 分钟"; enabled: !Pdf.busy
                onActivated: Pdf.requestTimeoutMinutes = minutes[currentIndex]
            }
        }
        UiText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "每段请求连续无响应时的等待上限，收到数据后重新计时。可随时暂停。"; color: Theme.secondary; font.pixelSize: 11 }
        RowLayout {
            Layout.fillWidth: true
            UiButton { objectName: "pdfManageProviders"; text: "管理 AI 服务"; compact: true; kind: "ghost"; onClicked: { dialog.close(); dialog.manageProviders() } }
            Item { Layout.fillWidth: true }
            UiButton { objectName: "pdfSettingsDone"; text: "完成"; compact: true; kind: "primary"; onClicked: dialog.close() }
        }
    }
}
