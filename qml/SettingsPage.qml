import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    signal editProvider(string id, int preset)
    spacing: 18
    RowLayout {
        Layout.fillWidth: true
        ColumnLayout {
            spacing: 5
            UiText { text: "偏好设置"; font.pixelSize: 22; font.weight: Font.DemiBold }
            UiText { text: "用习惯的模型，按自己的方式理解世界。"; color: Theme.secondary; font.pixelSize: 12 }
        }
        Item { Layout.fillWidth: true }
        UiButton { text: "添加 AI 服务"; iconName: "plus"; kind: "primary"; onClicked: page.editProvider("", 0) }
    }
    UiScrollView {
        id: settingsScroll
        objectName: "settingsScroll"
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            width: settingsScroll.availableWidth - 6; spacing: 16
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: providersColumn.implicitHeight + 36
                radius: 14; color: Theme.surface; border.color: Theme.border
                ColumnLayout {
                    id: providersColumn
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 18 }
                    spacing: 14
                    RowLayout {
                        Layout.fillWidth: true
                        Icon { name: "sparkles"; color: Theme.accent; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
                        UiText { text: "AI 服务"; font.pixelSize: 15; font.weight: Font.DemiBold }
                        Item { Layout.fillWidth: true }
                        UiText { text: App.providers.length + " 个配置"; font.pixelSize: 11; color: Theme.muted }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                    ColumnLayout {
                        visible: App.providers.length === 0
                        Layout.fillWidth: true; Layout.topMargin: 10; Layout.bottomMargin: 10; spacing: 12
                        UiText { text: "你的词典，由你选择的 AI 驱动"; font.pixelSize: 19; font.weight: Font.DemiBold }
                        UiText { text: "连接本地模型，或填入云端服务的地址和 API Key。\n配置完成后，可随时在翻译工作台切换。"; color: Theme.secondary; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.7; Layout.fillWidth: true }
                        Flow {
                            Layout.fillWidth: true; Layout.topMargin: 10; spacing: 9
                            Repeater {
                                model: [{name: "Ollama", local: true, preset: 0}, {name: "LM Studio", local: true, preset: 1}, {name: "OpenAI", local: false, preset: 2}, {name: "Responses", local: false, preset: 8}, {name: "DeepSeek", local: false, preset: 3}, {name: "通义千问", local: false, preset: 4}, {name: "Claude", local: false, preset: 5}, {name: "Gemini", local: false, preset: 6}]
                                delegate: UiButton {
                                    required property var modelData
                                    text: modelData.name; iconName: modelData.local ? "server" : "cloud"
                                    onClicked: page.editProvider("", modelData.preset)
                                }
                            }
                        }
                    }
                    Repeater {
                        model: App.providers
                        delegate: Rectangle {
                            id: serviceRow
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: providerContent.implicitHeight + 30
                            radius: 10; color: Theme.subtle
                            border.color: modelData.id === App.selectedProviderId ? Theme.accent : Theme.border
                            RowLayout {
                                id: providerContent
                                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 15 }
                                spacing: 15
                                Rectangle {
                                    implicitWidth: 42; implicitHeight: 42; radius: 10; color: Theme.accentSoft
                                    Icon { anchors.centerIn: parent; name: serviceRow.modelData.protocol === "ollama" || serviceRow.modelData.baseUrl.indexOf("localhost") >= 0 || serviceRow.modelData.baseUrl.indexOf("127.0.0.1") >= 0 ? "server" : "cloud"; color: Theme.accent; width: 22; height: 22 }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 5
                                    RowLayout {
                                        UiText { text: serviceRow.modelData.name; font.pixelSize: 14; font.weight: Font.DemiBold }
                                        UiText { text: "当前使用"; color: Theme.accent; font.pixelSize: 10; visible: serviceRow.modelData.id === App.selectedProviderId }
                                    }
                                    UiText { text: serviceRow.modelData.model + " · " + serviceRow.modelData.baseUrl; color: Theme.secondary; font.family: "Segoe UI"; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideMiddle }
                                }
                                UiButton { text: "使用"; kind: "soft"; compact: true; visible: serviceRow.modelData.id !== App.selectedProviderId; onClicked: App.selectProvider(serviceRow.modelData.id) }
                                UiButton { iconName: "edit"; hint: "编辑服务"; kind: "ghost"; compact: true; onClicked: page.editProvider(serviceRow.modelData.id, 0) }
                                UiButton { iconName: "trash"; hint: "移除服务"; kind: "ghost"; compact: true; onClicked: { removeDialog.providerId = serviceRow.modelData.id; removeDialog.providerName = serviceRow.modelData.name; removeDialog.open() } }
                            }
                        }
                    }
                }
            }
            DesktopSettings { Layout.fillWidth: true }
            Rectangle {
                Layout.fillWidth: true; implicitHeight: appearanceColumn.implicitHeight + 36
                radius: 14; color: Theme.surface; border.color: Theme.border
                ColumnLayout {
                    id: appearanceColumn
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 18 }
                    spacing: 16
                    RowLayout { spacing: 10; Icon { name: "sun"; color: Theme.accent; Layout.preferredWidth: 20; Layout.preferredHeight: 20 } UiText { text: "外观"; font.pixelSize: 15; font.weight: Font.DemiBold } }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        Repeater {
                            model: [{label: "浅色", value: "light", icon: "sun", bg: "#f6f8f6", side: "#e5ede6", line: "#d3e1d6", card: "#ffffff"}, {label: "深色", value: "dark", icon: "moon", bg: "#19221d", side: "#2a3c30", line: "#49614f", card: "#27332b"}, {label: "跟随系统", value: "system", icon: "monitor", bg: "#e5ede6", side: "#c5d4c8", line: "#9bb6a0", card: "#f6f8f6"}]
                            delegate: Button {
                                id: themeButton
                                required property var modelData
                                Layout.fillWidth: true; implicitHeight: 112
                                padding: 12
                                onClicked: App.theme = modelData.value
                                background: Rectangle { radius: 10; color: Theme.surface; border.width: App.theme === themeButton.modelData.value ? 2 : 1; border.color: App.theme === themeButton.modelData.value ? Theme.accent : Theme.border }
                                contentItem: ColumnLayout {
                                    spacing: 12
                                    Rectangle {
                                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 5; color: themeButton.modelData.bg
                                        Rectangle { width: parent.width * 0.22; height: parent.height; color: themeButton.modelData.side; radius: 4 }
                                        Rectangle { x: parent.width * 0.29; y: 12; width: parent.width * 0.6; height: 5; radius: 2; color: themeButton.modelData.line }
                                        Rectangle { x: parent.width * 0.29; y: 26; width: parent.width * 0.6; height: parent.height - 36; radius: 4; color: themeButton.modelData.card }
                                    }
                                    RowLayout {
                                        Icon { name: themeButton.modelData.icon; color: Theme.secondary; Layout.preferredWidth: 15; Layout.preferredHeight: 15 }
                                        UiText { text: themeButton.modelData.label; font.pixelSize: 12 }
                                        Item { Layout.fillWidth: true }
                                        Icon { visible: App.theme === themeButton.modelData.value; name: "check"; color: Theme.accent; Layout.preferredWidth: 16; Layout.preferredHeight: 16 }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; implicitHeight: dataColumn.implicitHeight + 36
                radius: 14; color: Theme.surface; border.color: Theme.border
                ColumnLayout {
                    id: dataColumn
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 18 }
                    spacing: 12
                    RowLayout { spacing: 10; Icon { name: "shield"; color: Theme.accent; Layout.preferredWidth: 20; Layout.preferredHeight: 20 } UiText { text: "数据与隐私"; font.pixelSize: 15; font.weight: Font.DemiBold } }
                    UiText { text: "查询历史和生词本存储在本机，API Key 使用 Windows 用户级加密。翻译和连接测试只访问你配置的服务；使用云端服务时，原文会发送给该服务商。"; color: Theme.secondary; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap; lineHeight: 1.6 }
                    UiText { text: "数据目录：" + App.dataDirectory; color: Theme.muted; font.pixelSize: 10; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                }
            }
            UiText { text: "译意 YIYI  /  " + Qt.application.version + "     ·     C++ & Qt Quick"; color: Theme.muted; font.family: "Segoe UI"; font.pixelSize: 11; Layout.topMargin: 2; Layout.bottomMargin: 8 }
        }
    }
    Dialog {
        id: removeDialog
        parent: Overlay.overlay
        property string providerId: ""
        property string providerName: ""
        anchors.centerIn: Overlay.overlay
        width: 380; padding: 24; modal: true
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 20
            UiText { text: "移除「" + removeDialog.providerName + "」？"; font.pixelSize: 17; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.Wrap }
            UiText { text: "将删除此服务的配置和保存的凭据。已有历史记录会保留。"; color: Theme.secondary; Layout.fillWidth: true; wrapMode: Text.Wrap; lineHeight: 1.5 }
            RowLayout {
                Item { Layout.fillWidth: true }
                UiButton { text: "取消"; onClicked: removeDialog.close() }
                UiButton { text: "移除服务"; kind: "danger"; onClicked: { App.removeProvider(removeDialog.providerId); removeDialog.close() } }
            }
        }
    }
}
