import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "providerDialog"
    parent: Overlay.overlay
    property string editingId: ""
    property bool storedKey: false
    property string formError: ""
    readonly property var protocols: ["openai", "responses", "ollama", "anthropic", "gemini"]
    readonly property bool reasoningConfigurable: protocols[protocol.currentIndex] === "openai" || protocols[protocol.currentIndex] === "responses"
    readonly property var reasoningLevels: [
        {label: "跟随服务（默认）", value: ""},
        {label: "关闭思考 · none", value: "none"},
        {label: "低 · low", value: "low"},
        {label: "中 · medium", value: "medium"},
        {label: "高 · high", value: "high"},
        {label: "很高 · xhigh", value: "xhigh"},
        {label: "最高 · max", value: "max"}
    ]
    readonly property var dictionaryModes: [
        {label: "简明词条（默认）", value: "brief"},
        {label: "完整词条", value: "detailed"}
    ]
    onReasoningConfigurableChanged: if (!reasoningConfigurable && reasoningLevel) reasoningLevel.currentIndex = 0
    readonly property var presets: [
        {name: "Ollama", protocol: "ollama", baseUrl: "http://localhost:11434", model: "qwen3:8b"},
        {name: "LM Studio", protocol: "openai", baseUrl: "http://localhost:1234/v1", model: ""},
        {name: "OpenAI", protocol: "openai", baseUrl: "https://api.openai.com/v1", model: ""},
        {name: "DeepSeek", protocol: "openai", baseUrl: "https://api.deepseek.com/v1", model: "deepseek-chat"},
        {name: "通义千问", protocol: "openai", baseUrl: "https://dashscope.aliyuncs.com/compatible-mode/v1", model: "qwen-plus"},
        {name: "Claude", protocol: "anthropic", baseUrl: "https://api.anthropic.com", model: ""},
        {name: "Gemini", protocol: "gemini", baseUrl: "https://generativelanguage.googleapis.com/v1beta", model: ""},
        {name: "自定义服务", protocol: "openai", baseUrl: "", model: ""},
        {name: "OpenAI Responses", protocol: "responses", baseUrl: "https://api.openai.com/v1", model: ""}
    ]
    anchors.centerIn: Overlay.overlay
    width: 550
    height: Math.min(680, parent ? parent.height - 32 : 680)
    padding: 20
    modal: true
    closePolicy: Popup.CloseOnEscape
    background: Rectangle { color: Theme.surface; radius: 16; border.color: Theme.border }
    Overlay.modal: Rectangle { color: Theme.dark ? "#aa080d0a" : "#6627372d" }

    function applyPreset(index) {
        var p = presets[index]
        serviceName.text = p.name
        protocol.currentIndex = protocols.indexOf(p.protocol)
        address.text = p.baseUrl
        modelName.text = p.model
        reasoningLevel.currentIndex = 0
        dictionaryMode.currentIndex = 0
        apiKey.text = ""
        clearKey.checked = storedKey
        formError = ""
        App.resetTest()
    }
    function edit(id, presetIndex) {
        editingId = id || ""
        formError = ""
        App.resetTest()
        apiKey.text = ""
        showKey.checked = false
        clearKey.checked = false
        reasoningLevel.currentIndex = 0
        dictionaryMode.currentIndex = 0
        if (editingId.length) {
            var p = App.providerDraft(editingId)
            storedKey = !!p.hasKey
            serviceName.text = p.name || ""
            protocol.currentIndex = protocols.indexOf(p.protocol)
            address.text = p.baseUrl || ""
            modelName.text = p.model || ""
            dictionaryMode.currentIndex = p.dictionaryMode === "detailed" ? 1 : 0
            if (reasoningConfigurable)
                reasoningLevel.currentIndex = Math.max(0, reasoningLevels.findIndex(function(level) { return level.value === (p.reasoningEffort || "") }))
            templatePicker.currentIndex = -1
        } else {
            storedKey = false
            templatePicker.currentIndex = presetIndex === undefined ? 0 : presetIndex
            applyPreset(templatePicker.currentIndex)
        }
        open()
    }
    function draft() {
        return { id: editingId, name: serviceName.text, protocol: protocols[protocol.currentIndex], baseUrl: address.text, model: modelName.text, apiKey: apiKey.text, clearKey: clearKey.checked,
            reasoningEffort: reasoningConfigurable ? (reasoningLevel.currentValue || "") : "",
            dictionaryMode: dictionaryMode.currentValue || "brief" }
    }
    function changed() { if (App.testMessage.length) App.resetTest(); formError = "" }
    onClosed: { App.resetTest(); apiKey.text = "" }

    header: Item {
        implicitHeight: 76
        RowLayout {
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 20 }
            ColumnLayout {
                spacing: 7; Layout.fillWidth: true
                UiText { text: dialog.editingId.length ? "编辑 AI 服务" : "连接一个 AI 服务"; font.pixelSize: 21; font.weight: Font.DemiBold }
                UiText { text: "把你喜欢的模型，变成随手可用的词典。"; color: Theme.secondary; font.pixelSize: 12 }
            }
            UiButton { iconName: "close"; hint: "关闭"; kind: "ghost"; compact: true; onClicked: dialog.close() }
        }
    }
    contentItem: UiScrollView {
        id: formScroll
        objectName: "providerFormScroll"
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            width: formScroll.availableWidth; spacing: 10
            UiText { text: "快速选择"; font.pixelSize: 12; font.weight: Font.DemiBold }
            UiCombo {
                id: templatePicker
                objectName: "presetPicker"
                Layout.fillWidth: true
                model: dialog.presets; textRole: "name"
                displayText: currentIndex < 0 ? "选择预设会替换下方服务信息" : currentText
                onActivated: dialog.applyPreset(currentIndex)
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 14; Layout.topMargin: 7
                ColumnLayout {
                    Layout.fillWidth: true; Layout.preferredWidth: 250; spacing: 8
                    UiText { text: "服务名称"; font.pixelSize: 12; font.weight: Font.DemiBold }
                    UiField { id: serviceName; objectName: "serviceNameField"; Layout.fillWidth: true; placeholderText: "例如：我的本地模型"; onTextEdited: dialog.changed(); maximumLength: 60 }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.preferredWidth: 220; spacing: 8
                    UiText { text: "接口协议"; font.pixelSize: 12; font.weight: Font.DemiBold }
                    UiCombo { id: protocol; objectName: "protocolPicker"; Layout.fillWidth: true; implicitHeight: 44; model: ["OpenAI Chat Completions", "OpenAI Responses", "Ollama 原生", "Anthropic", "Gemini"]; onActivated: dialog.changed() }
                }
            }
            UiText { text: "服务地址"; font.pixelSize: 12; font.weight: Font.DemiBold; Layout.topMargin: 8 }
            UiField { id: address; objectName: "endpointField"; Layout.fillWidth: true; placeholderText: "https://api.example.com/v1"; onTextEdited: dialog.changed(); maximumLength: 2048 }
            UiText {
                objectName: "endpointHint"
                text: dialog.protocols[protocol.currentIndex] === "responses" ? "填写以 /v1 结尾的基础地址，程序会请求 /responses；也可填写完整的 /v1/responses 地址。"
                    : dialog.protocols[protocol.currentIndex] === "openai" ? "此协议请求 /chat/completions。服务仅支持 /responses 时，请选择 OpenAI Responses。"
                    : dialog.protocols[protocol.currentIndex] === "ollama" ? "填写 Ollama 主机与端口，程序会请求 /api/chat。"
                    : dialog.protocols[protocol.currentIndex] === "anthropic" ? "填写 Anthropic 基础地址，程序会请求 /v1/messages。"
                    : "填写 Gemini API 基础地址，例如 https://generativelanguage.googleapis.com/v1beta。"
                color: Theme.muted; font.pixelSize: 10; Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 7
                UiText { text: "API Key"; font.pixelSize: 12; font.weight: Font.DemiBold }
                Item { Layout.fillWidth: true }
                UiText { text: dialog.storedKey ? "已加密保存，留空可保留" : "本地服务通常可留空"; color: Theme.muted; font.pixelSize: 10 }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                UiField { id: apiKey; objectName: "apiKeyField"; Layout.fillWidth: true; placeholderText: dialog.storedKey ? "输入新 Key 以替换已保存凭据" : "填入服务商提供的 API Key"; echoMode: showKey.checked ? TextInput.Normal : TextInput.Password; onTextEdited: dialog.changed(); maximumLength: 4096 }
                UiButton { id: showKey; checkable: true; iconName: "eye"; hint: checked ? "隐藏 API Key" : "显示 API Key"; kind: checked ? "soft" : "outline"; implicitHeight: 44 }
            }
            CheckBox {
                id: clearKey
                visible: dialog.storedKey
                text: "清除已保存的 API Key（适用于无需密钥的服务）"
                font.family: Theme.font; font.pixelSize: 10
                palette.windowText: Theme.secondary; palette.text: Theme.secondary; palette.highlight: Theme.accent
                onToggled: dialog.changed()
            }
            UiText { text: "模型名称"; font.pixelSize: 12; font.weight: Font.DemiBold; Layout.topMargin: 8 }
            UiField { id: modelName; objectName: "modelField"; Layout.fillWidth: true; placeholderText: "输入服务端实际提供的模型 ID"; onTextEdited: dialog.changed(); maximumLength: 200 }
            UiText { text: "请确认模型已下载或已开通。预设名称可按你的实际环境修改。"; color: Theme.muted; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true }
            ColumnLayout {
                visible: dialog.reasoningConfigurable
                Layout.fillWidth: true; Layout.topMargin: 8; spacing: 8
                UiText { text: "思考等级（可选）"; font.pixelSize: 12; font.weight: Font.DemiBold }
                UiCombo {
                    id: reasoningLevel
                    objectName: "reasoningPicker"
                    Layout.fillWidth: true
                    model: dialog.reasoningLevels
                    textRole: "label"; valueRole: "value"
                    currentIndex: 0
                    Accessible.name: "思考等级（可选）"
                    onActivated: dialog.changed()
                }
                UiText {
                    objectName: "reasoningHint"
                    text: reasoningLevel.currentIndex === 0 ? "不确定是否支持时，请保持跟随服务；不会发送思考参数。"
                        : reasoningLevel.currentValue === "none" ? "关闭思考也需要模型支持。不支持此功能的本地模型请选「跟随服务」。"
                        : "低等级通常更快；支持的等级因模型而异，可测试连接检查服务是否接受。"
                    color: Theme.secondary; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.topMargin: 8; spacing: 8
                UiText { text: "词典内容"; font.pixelSize: 12; font.weight: Font.DemiBold }
                UiCombo {
                    id: dictionaryMode
                    objectName: "dictionaryModePicker"
                    Layout.fillWidth: true
                    model: dialog.dictionaryModes
                    textRole: "label"; valueRole: "value"
                    currentIndex: 0
                    Accessible.name: "词典内容"
                    onActivated: dialog.changed()
                }
                UiText {
                    text: dictionaryMode.currentIndex === 0 ? "生成词性和常见译义，适合本地小模型；不生成音标、例句等扩展内容。"
                        : "生成释义、音标、例句和搭配，对模型能力要求较高，等待时间也更长。"
                    color: Theme.secondary; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.topMargin: 8
                implicitHeight: security.implicitHeight + 24
                color: Theme.subtle; radius: 8
                RowLayout {
                    id: security
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                    spacing: 10
                    Icon { name: "shield"; color: Theme.accent; Layout.preferredWidth: 17; Layout.preferredHeight: 17; Layout.alignment: Qt.AlignTop }
                    UiText { text: "API Key 通过 Windows 用户凭据加密。测试连接会发送简短文本；测试识图会发送随机文字图片，验证模型是否真正读取图片。"; font.pixelSize: 10; color: Theme.secondary; wrapMode: Text.Wrap; lineHeight: 1.5; Layout.fillWidth: true }
                }
            }
            UiText {
                visible: dialog.formError.length > 0 || App.testMessage.length > 0
                text: dialog.formError.length ? dialog.formError : App.testMessage
                color: dialog.formError.length || (!App.testSuccess && !App.testBusy) ? Theme.danger : Theme.accent
                font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap; lineHeight: 1.4; Layout.topMargin: 4
            }
        }
    }
    footer: Item {
        implicitHeight: 72
        RowLayout {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 18 }
            UiButton { objectName: "testProviderButton"; text: App.testBusy ? "取消测试" : "测试连接"; iconName: App.testBusy ? "stop" : "link"; onClicked: App.testBusy ? App.resetTest() : App.testProvider(dialog.draft()) }
            UiButton { objectName: "testVisionButton"; text: "测试识图"; compact: true; enabled: !App.testBusy; onClicked: App.testVisionProvider(dialog.draft()) }
            Item { Layout.fillWidth: true }
            UiButton { text: "取消"; kind: "ghost"; onClicked: dialog.close() }
            UiButton { objectName: "saveProviderButton"; text: "保存服务"; kind: "primary"; iconName: "check"; enabled: !App.testBusy; onClicked: { dialog.formError = App.saveProvider(dialog.draft()); if (!dialog.formError.length) dialog.close() } }
        }
    }
}
