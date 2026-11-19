import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    property bool favoritesOnly: false
    signal startTranslating()
    readonly property var records: favoritesOnly ? App.favorites : App.history
    readonly property var filtered: {
        var needle = search.text.trim().toLowerCase()
        if (!needle.length) return records
        return records.filter(function(r) { return r.input.toLowerCase().indexOf(needle) >= 0 || r.output.toLowerCase().indexOf(needle) >= 0 })
    }
    spacing: 18
    RowLayout {
        Layout.fillWidth: true
        ColumnLayout {
            spacing: 5
            UiText { text: page.favoritesOnly ? "生词本" : "查询历史"; font.pixelSize: 22; font.weight: Font.DemiBold }
            UiText { text: page.favoritesOnly ? "把不期而遇的好词，变成熟悉的表达。" : "每一次好奇，都有迹可循。"; color: Theme.secondary; font.pixelSize: 12 }
        }
        Item { Layout.fillWidth: true }
        UiText { text: page.records.length + " 条记录"; color: Theme.secondary; font.pixelSize: 12 }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 12
        UiField {
            id: search
            Layout.fillWidth: true; placeholderText: page.favoritesOnly ? "搜索收藏的词条与译文…" : "搜索原文或翻译结果…"
            leftPadding: 42
            Icon { name: "search"; x: 14; anchors.verticalCenter: parent.verticalCenter; width: 17; height: 17; color: Theme.muted }
            Accessible.name: "搜索记录"
        }
        UiButton { iconName: "plus"; text: "开始新查询"; onClicked: { App.clear(); page.startTranslating() } }
    }
    ListView {
        id: recordList
        objectName: page.favoritesOnly ? "favoritesList" : "historyList"
        Layout.fillWidth: true; Layout.fillHeight: true
        visible: page.filtered.length > 0
        model: page.filtered
        clip: true; spacing: 12
        ScrollBehavior { flickable: recordList }
        ScrollBar.vertical: UiScrollBar { objectName: recordList.objectName + "Bar" }
        delegate: Rectangle {
            id: recordCard
            objectName: (page.favoritesOnly ? "favoriteRecord-" : "historyRecord-") + modelData.id
            required property var modelData
            width: recordList.width - 16
            // A fixed two-line preview keeps ListView's content-height estimate
            // stable even when long and short records are interleaved.
            height: 132
            radius: 12; color: recordMouse.containsMouse ? Theme.subtle : Theme.surface; border.color: activeFocus ? Theme.accent : Theme.border
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: modelData.input
            Keys.onReturnPressed: App.openRecord(modelData.id)
            MouseArea { id: recordMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: App.openRecord(recordCard.modelData.id) }
            RowLayout {
                id: rowContent
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 18 }
                spacing: 18
                Rectangle {
                    Layout.alignment: Qt.AlignTop; implicitWidth: 42; implicitHeight: 42; radius: 10; color: Theme.accentSoft
                    Icon { anchors.centerIn: parent; name: recordCard.modelData.kind === "dictionary" ? "book" : "translate"; color: Theme.accent; width: 20; height: 20 }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 8
                    UiText { text: recordCard.modelData.input; Layout.fillWidth: true; elide: Text.ElideRight; maximumLineCount: 1; font.pixelSize: 16; font.weight: Font.DemiBold }
                    UiText { text: recordCard.modelData.kind === "dictionary" && recordCard.modelData.dictionary.definitions ? recordCard.modelData.dictionary.definitions.map(function(d) { return d.meaning }).join("；") : recordCard.modelData.output; Layout.fillWidth: true; Layout.preferredHeight: 36; wrapMode: Text.Wrap; elide: Text.ElideRight; maximumLineCount: 2; color: Theme.secondary; font.pixelSize: 12; lineHeight: 1.5 }
                    UiText { text: recordCard.modelData.dateLabel + "    ·    " + recordCard.modelData.provider + "    ·    " + recordCard.modelData.target; color: Theme.muted; font.pixelSize: 10; Layout.fillWidth: true; elide: Text.ElideRight }
                }
                UiButton { iconName: recordCard.modelData.favorite ? "check" : "star"; hint: recordCard.modelData.favorite ? "取消收藏" : "加入生词本"; kind: recordCard.modelData.favorite ? "soft" : "ghost"; compact: true; onClicked: App.setRecordFavorite(recordCard.modelData.id, !recordCard.modelData.favorite) }
                UiButton { iconName: "trash"; hint: "删除记录"; kind: "ghost"; compact: true; onClicked: { deleteDialog.recordId = recordCard.modelData.id; deleteDialog.open() } }
            }
        }
    }
    Rectangle {
        visible: page.filtered.length === 0
        Layout.fillWidth: true; Layout.fillHeight: true
        color: Theme.surface; border.color: Theme.border; radius: 14
        ColumnLayout {
            anchors.centerIn: parent; width: Math.min(parent.width - 60, 420); spacing: 14
            Image { source: "qrc:/assets/empty.svg"; Layout.preferredWidth: 170; Layout.preferredHeight: 134; Layout.alignment: Qt.AlignHCenter; opacity: Theme.dark ? 0.7 : 1; sourceSize.width: 340; sourceSize.height: 268 }
            UiText { text: search.text.length ? "没有找到匹配的记录" : page.favoritesOnly ? "为喜欢的表达，留一个位置" : "你的语言足迹，从这里开始"; font.pixelSize: 18; Layout.fillWidth: true; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
            UiText { text: search.text.length ? "换一个关键词，或清空搜索条件再试试。" : page.favoritesOnly ? "查询后点击星标，便能把词条或译文收进生词本。\n积少成多，让每一次理解都留下来。" : "完成一次查词或翻译后，结果会自动保存。\n随时回来重读，无需再次请求 AI。"; color: Theme.secondary; Layout.fillWidth: true; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 12; lineHeight: 1.8 }
            UiButton { text: search.text.length ? "清空搜索" : "去翻译工作台"; iconName: "arrowRight"; kind: "soft"; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 6; onClicked: search.text.length ? search.clear() : page.startTranslating() }
        }
    }
    RowLayout {
        Icon { name: "shield"; color: Theme.muted; Layout.preferredWidth: 13; Layout.preferredHeight: 13 }
        UiText { text: "保存在本机 · 点击记录可重新打开完整结果"; color: Theme.muted; font.pixelSize: 10 }
    }
    Dialog {
        id: deleteDialog
        parent: Overlay.overlay
        property double recordId: -1
        anchors.centerIn: Overlay.overlay
        width: 380; padding: 24; modal: true
        background: Rectangle { color: Theme.surface; radius: 12; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 20
            UiText { text: "删除这条记录？"; font.pixelSize: 18; font.weight: Font.DemiBold }
            UiText { text: "该记录也会从生词本中移除，删除后无法恢复。"; color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true; lineHeight: 1.5 }
            RowLayout {
                Item { Layout.fillWidth: true }
                UiButton { text: "取消"; onClicked: deleteDialog.close() }
                UiButton { text: "删除记录"; kind: "danger"; onClicked: { App.deleteRecord(deleteDialog.recordId); deleteDialog.close() } }
            }
        }
    }
}
