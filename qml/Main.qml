import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: window
    width: 1000; height: 680
    minimumWidth: 900; minimumHeight: 600
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowSystemMenuHint | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint
    title: "译意 · AI 翻译词典"
    color: Theme.background
    font.family: Theme.font
    property int currentPage: initialPage === "history" ? 1 : initialPage === "favorites" ? 2 : initialPage === "settings" || initialPage === "editor" ? 3 : initialPage === "documents" ? 4 : initialPage === "pdf" ? 5 : 0
    property bool readingMode: false
    property bool pdfEverOpened: false
    onCurrentPageChanged: {
        if (currentPage !== 5) readingMode = false
        else pdfEverOpened = true
    }
    onClosing: function(close) { if (Desktop.handleClose()) close.accepted = false }
    SelectionBubble {}

    function toggleMaximized() {
        if (visibility === Window.Maximized) showNormal()
        else showMaximized()
    }

    RowLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            id: sidebar
            visible: !window.readingMode
            Layout.preferredWidth: 176; Layout.fillHeight: true
            color: Theme.sidebar
            Rectangle { anchors.right: parent.right; height: parent.height; width: 1; color: Theme.border }
            WindowDragArea {
                objectName: "sidebarDragArea"
                anchors.fill: parent
                targetWindow: window
            }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 8
                RowLayout {
                    spacing: 10; Layout.topMargin: 10; Layout.leftMargin: 8; Layout.bottomMargin: 24
                    Image { source: "qrc:/assets/logo.svg"; sourceSize.width: 72; sourceSize.height: 72; Layout.preferredWidth: 36; Layout.preferredHeight: 36 }
                    ColumnLayout {
                        spacing: 1
                        UiText { text: "译意"; font.pixelSize: 21; font.weight: Font.DemiBold; font.letterSpacing: 2 }
                        UiText { text: "Y I Y I"; color: Theme.secondary; font.family: "Segoe UI"; font.pixelSize: 9; font.letterSpacing: 1.8 }
                    }
                }
                UiText { text: "工作空间"; color: Theme.muted; font.pixelSize: 10; Layout.leftMargin: 14; Layout.bottomMargin: 7 }
                Repeater {
                    model: [{label: "翻译工作台", icon: "translate", page: 0}, {label: "查询历史", icon: "history", page: 1}, {label: "生词本", icon: "book", page: 2}, {label: "文档翻译", icon: "document", page: 4}, {label: "PDF 翻译", icon: "document", page: 5}]
                    delegate: Button {
                        id: nav
                        required property var modelData
                        objectName: "navigation-" + modelData.page
                        Layout.fillWidth: true; implicitHeight: 42
                        leftPadding: 14; rightPadding: 12; hoverEnabled: true
                        onClicked: window.currentPage = modelData.page
                        background: Rectangle { radius: 9; color: window.currentPage === nav.modelData.page ? Theme.accentSoft : nav.hovered ? Theme.hover : "transparent"; border.width: nav.activeFocus ? 1 : 0; border.color: Theme.accent }
                        contentItem: RowLayout {
                            spacing: 12
                            Icon { name: nav.modelData.icon; color: window.currentPage === nav.modelData.page ? Theme.accent : Theme.secondary; Layout.preferredWidth: 19; Layout.preferredHeight: 19 }
                            UiText { text: nav.modelData.label; color: window.currentPage === nav.modelData.page ? Theme.accent : Theme.secondary; font.pixelSize: 12; font.weight: window.currentPage === nav.modelData.page ? Font.DemiBold : Font.Normal; Layout.fillWidth: true }
                            UiText { visible: nav.modelData.page === 2 && App.favorites.length > 0; text: App.favorites.length; color: Theme.muted; font.family: "Segoe UI"; font.pixelSize: 10 }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; Layout.topMargin: 12; Layout.bottomMargin: 10; implicitHeight: 1; color: Theme.border }
                Button {
                    id: settingsNav
                    objectName: "navigation-3"
                    Layout.fillWidth: true; implicitHeight: 42; leftPadding: 14; hoverEnabled: true
                    onClicked: window.currentPage = 3
                    background: Rectangle { radius: 9; color: window.currentPage === 3 ? Theme.accentSoft : settingsNav.hovered ? Theme.hover : "transparent"; border.width: settingsNav.activeFocus ? 1 : 0; border.color: Theme.accent }
                    contentItem: RowLayout {
                        spacing: 12
                        Icon { name: "settings"; color: window.currentPage === 3 ? Theme.accent : Theme.secondary; Layout.preferredWidth: 19; Layout.preferredHeight: 19 }
                        UiText { text: "偏好设置"; Layout.fillWidth: true; font.pixelSize: 12; color: window.currentPage === 3 ? Theme.accent : Theme.secondary; font.weight: window.currentPage === 3 ? Font.DemiBold : Font.Normal }
                    }
                }
                Item { objectName: "sidebarDragSpace"; Layout.fillHeight: true; Layout.fillWidth: true }
                Rectangle {
                    visible: window.height >= 760
                    Layout.fillWidth: true; implicitHeight: connectColumn.implicitHeight + 30
                    radius: 10; color: Theme.surface; border.color: Theme.border
                    ColumnLayout {
                        id: connectColumn
                        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 15 }
                        spacing: 10
                        RowLayout {
                            Icon { name: "sparkles"; color: Theme.accent; Layout.preferredWidth: 17; Layout.preferredHeight: 17 }
                            UiText { text: "自由连接，自在表达"; font.pixelSize: 10; font.weight: Font.DemiBold }
                        }
                        UiText { text: App.providers.length ? App.providerName + "\n" + App.modelName : "本地模型或云端 AI，\n让词典更懂你的需要。"; color: Theme.secondary; font.pixelSize: 10; lineHeight: 1.7; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight; Layout.fillWidth: true }
                        UiButton { text: "管理 AI 服务"; iconName: "arrowRight"; compact: true; kind: "soft"; Layout.fillWidth: true; onClicked: window.currentPage = 3 }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.topMargin: 17; spacing: 2
                    Repeater {
                        model: [{value: "light", icon: "sun", label: "浅色模式"}, {value: "dark", icon: "moon", label: "深色模式"}, {value: "system", icon: "monitor", label: "跟随系统"}]
                        delegate: UiButton {
                            required property var modelData
                            iconName: modelData.icon; hint: modelData.label; compact: true
                            objectName: "theme-" + modelData.value
                            focusPolicy: Qt.TabFocus
                            kind: App.theme === modelData.value ? "soft" : "ghost"
                            onClicked: App.theme = modelData.value
                        }
                    }
                    Item { Layout.fillWidth: true }
                    UiText { text: "v" + Qt.application.version; font.family: "Segoe UI"; font.pixelSize: 9; color: Theme.muted }
                }
            }
        }
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.margins: 20
            Layout.topMargin: 44
            currentIndex: window.currentPage
            TranslatePage { onOpenSettings: window.currentPage = 3; onScreenshotRequested: Capture.capture() }
            LibraryPage { onStartTranslating: window.currentPage = 0 }
            LibraryPage { favoritesOnly: true; onStartTranslating: window.currentPage = 0 }
            SettingsPage { onEditProvider: function(id, preset) { providerDialog.edit(id, preset) } }
            DocumentPage { id: documentPage; onOpenSettings: window.currentPage = 3 }
            Item {
                Loader {
                    id: pdfLoader
                    anchors.fill: parent
                    active: window.currentPage === 5 || window.pdfEverOpened
                    sourceComponent: PdfPage {
                        expanded: window.readingMode
                        onOpenSettings: window.currentPage = 3
                        onToggleReading: window.readingMode = !window.readingMode
                    }
                }
            }
        }
    }
    Item {
        anchors { top: parent.top; right: parent.right; left: parent.left; leftMargin: sidebar.visible ? sidebar.width : 0 }
        height: 36
        WindowDragArea {
            objectName: "headerDragArea"
            anchors.fill: parent
            targetWindow: window
        }
        WindowControls { anchors.right: parent.right; targetWindow: window }
    }
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: window.visibility === Window.Maximized ? 0 : 1
        border.color: Theme.border
        enabled: false
    }
    WindowResizeHandles { anchors.fill: parent; targetWindow: window; z: 20 }
    FileDropArea {
        anchors.fill: parent
        // Above popup editors for file drags; no mouse handlers, so ordinary
        // selection, scrolling, title controls and resizing still pass through.
        z: Overlay.overlay.z + 1
        blocked: Overlay.overlay.visible || documentPage.fileDialogOpen || (pdfLoader.item ? pdfLoader.item.fileDialogOpen : false)
        onRejected: function(message) { toast.message = message; toastTimer.restart() }
    }
    // Keep popup ownership in the visual tree so their controls are destroyed
    // before the window's event delivery agent during shutdown.
    Item {
        ProviderDialog { id: providerDialog }
        ScreenshotDialog { id: screenshotDialog }
        FileReplaceDialog { id: fileReplaceDialog }
    }
    Rectangle {
        id: toast
        property string message: ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom; anchors.bottomMargin: 28
        width: Math.min(toastText.implicitWidth + 40, window.width - 100)
        height: toastText.implicitHeight + 24
        color: Theme.dark ? "#dce9df" : "#293f32"; radius: 10
        opacity: toastTimer.running ? 1 : 0
        visible: opacity > 0
        z: 10
        Behavior on opacity { NumberAnimation { duration: 160 } }
        UiText { id: toastText; text: toast.message; anchors.centerIn: parent; width: Math.min(implicitWidth, window.width - 140); wrapMode: Text.Wrap; color: Theme.dark ? "#253b2d" : "#ffffff"; font.pixelSize: 12 }
        Timer { id: toastTimer; interval: 3200 }
    }
    Connections {
        target: Capture
        function onReviewRequested() { screenshotDialog.open() }
    }
    Connections {
        target: FileImport
        function onOpened(kind) { window.currentPage = kind === "pdf" ? 5 : 4 }
        function onToast(message) { toast.message = message; toastTimer.restart() }
    }
    Connections {
        target: Documents
        function onToast(message) { toast.message = message; toastTimer.restart() }
    }
    Connections {
        target: Office
        function onToast(message) { toast.message = message; toastTimer.restart() }
    }
    Connections {
        target: Pdf
        function onToast(message) { toast.message = message; toastTimer.restart() }
    }
    Connections {
        target: Desktop
        function onToast(message) { toast.message = message; toastTimer.restart() }
        function onWorkspaceRequested() { window.currentPage = 0 }
        function onSettingsRequested() { window.currentPage = 3 }
        function onScreenshotRequested() { Capture.capture() }
    }
    Connections {
        target: App
        function onToast(message) { toast.message = message; toastTimer.restart() }
        function onConfigurationRequested() { window.currentPage = 3; providerDialog.edit("", 0) }
        function onRecordOpened() { window.currentPage = 0 }
    }
    Shortcut { sequence: "Ctrl+Return"; onActivated: if (!providerDialog.opened && window.currentPage === 0) App.translate() }
    Shortcut { sequence: "Ctrl+,"; onActivated: window.currentPage = 3 }
    Shortcut { sequence: "Ctrl+1"; onActivated: window.currentPage = 0 }
    Shortcut { sequence: "Ctrl+2"; onActivated: window.currentPage = 1 }
    Shortcut { sequence: "Ctrl+3"; onActivated: window.currentPage = 2 }
    Shortcut { sequence: "Ctrl+4"; onActivated: window.currentPage = 4 }
    Shortcut { sequence: "Ctrl+5"; onActivated: window.currentPage = 5 }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: if (!providerDialog.opened && !screenshotDialog.opened) Capture.capture() }
    Component.onCompleted: if (initialPage === "editor") Qt.callLater(function() { providerDialog.edit("", 0) })
}
