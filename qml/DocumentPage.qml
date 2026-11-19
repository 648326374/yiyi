import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    signal openSettings()
    property int formatTab: 0
    readonly property bool fileDialogOpen: txtPage.fileDialogOpen || officePage.fileDialogOpen
    spacing: 8
    RowLayout {
        Layout.fillWidth: true; spacing: 6
        UiButton { objectName: "documentTxtTab"; text: "TXT"; compact: true; kind: page.formatTab === 0 ? "soft" : "ghost"; onClicked: page.formatTab = 0 }
        UiButton { objectName: "documentOfficeTab"; text: "Word / PPT"; compact: true; kind: page.formatTab === 1 ? "soft" : "ghost"; onClicked: page.formatTab = 1 }
        Item { Layout.fillWidth: true }
        UiText { text: "打开或拖入文件"; color: Theme.muted; font.pixelSize: 10 }
    }
    StackLayout {
        Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: page.formatTab
        TxtDocumentPage { id: txtPage; onOpenSettings: page.openSettings() }
        OfficePage { id: officePage; onOpenSettings: page.openSettings() }
    }
    Connections {
        target: FileImport
        function onOpened(kind) { if (kind === "txt") page.formatTab = 0; else if (kind === "docx" || kind === "pptx") page.formatTab = 1 }
    }
}
