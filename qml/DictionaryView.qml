import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    property var entry: ({})
    property bool brief: false
    spacing: 18
    RowLayout {
        visible: !root.brief
        Layout.fillWidth: true
        ColumnLayout {
            spacing: 9
            Layout.fillWidth: true
            UiText { objectName: "dictionaryWord"; text: root.entry.word || ""; font.family: "Georgia"; font.pixelSize: 32; font.weight: Font.Normal; Layout.fillWidth: true; wrapMode: Text.Wrap }
            UiText { text: root.entry.phonetic || ""; visible: text.length > 0; color: Theme.secondary; font.family: "Segoe UI"; font.pixelSize: 15 }
        }
    }
    Rectangle {
        visible: posLabel.text.length > 0
        implicitWidth: posLabel.implicitWidth + 18
        implicitHeight: 26
        radius: 5; color: Theme.accentSoft
        UiText { id: posLabel; text: root.entry.partOfSpeech || ""; anchors.centerIn: parent; color: Theme.accent; font.pixelSize: 11 }
    }
    ColumnLayout {
        spacing: 14
        Layout.fillWidth: true
        Repeater {
            model: root.entry.definitions || []
            delegate: RowLayout {
                required property var modelData
                required property int index
                Layout.fillWidth: true; spacing: 13
                UiText {
                    objectName: "dictionaryPos-" + index
                    text: root.brief ? (modelData.partOfSpeech || "") : String(index + 1).padStart(2, "0")
                    visible: text.length > 0
                    color: Theme.accent; font.pixelSize: 13; font.family: "Segoe UI"; font.italic: root.brief
                    Layout.alignment: Qt.AlignTop; Layout.topMargin: 3
                    Layout.minimumWidth: root.brief ? 30 : 0
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 7
                    UiText { objectName: "dictionaryMeaning-" + index; text: modelData.meaning || ""; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 15; font.weight: Font.DemiBold; lineHeight: 1.4 }
                    UiText { text: modelData.detail || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary; lineHeight: 1.5; font.pixelSize: 12 }
                }
            }
        }
    }
    Rectangle {
        visible: (root.entry.examples || []).length > 0 || (root.entry.note || "").length > 0
            || (root.entry.collocations || []).length > 0 || (root.entry.synonyms || []).length > 0
        Layout.fillWidth: true; implicitHeight: 1; color: Theme.border
    }
    ColumnLayout {
        visible: (root.entry.examples || []).length > 0
        spacing: 16; Layout.fillWidth: true
        UiText { text: "语境例句"; font.pixelSize: 12; font.weight: Font.DemiBold; color: Theme.secondary }
        Repeater {
            model: root.entry.examples || []
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true; spacing: 13
                Rectangle { Layout.fillHeight: true; Layout.preferredWidth: 2; color: Theme.accentSoft; radius: 1 }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 5
                    UiText { text: modelData.source || ""; font.family: "Segoe UI"; font.pixelSize: 15; Layout.fillWidth: true; wrapMode: Text.Wrap; lineHeight: 1.35 }
                    UiText { text: modelData.translation || ""; color: Theme.secondary; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap; lineHeight: 1.4 }
                }
            }
        }
    }
    Rectangle {
        visible: (root.entry.note || "").length > 0
        Layout.fillWidth: true
        implicitHeight: noteRow.implicitHeight + 28
        radius: 9; color: Theme.accentSoft
        RowLayout {
            id: noteRow
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 14 }
            spacing: 10
            Icon { name: "sparkles"; color: Theme.accent; Layout.alignment: Qt.AlignTop; Layout.preferredWidth: 16; Layout.preferredHeight: 16; Layout.topMargin: 2 }
            UiText { text: root.entry.note || ""; Layout.fillWidth: true; wrapMode: Text.Wrap; lineHeight: 1.5; font.pixelSize: 12; color: Theme.accent }
        }
    }
    ColumnLayout {
        visible: (root.entry.collocations || []).length > 0
        Layout.fillWidth: true; spacing: 12
        UiText { text: "常见搭配"; font.pixelSize: 12; font.weight: Font.DemiBold; color: Theme.secondary }
        Repeater {
            model: root.entry.collocations || []
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                UiText { text: modelData.phrase || ""; font.family: "Segoe UI"; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.Wrap }
                UiText { text: modelData.meaning || ""; font.pixelSize: 12; color: Theme.secondary; Layout.maximumWidth: root.width * 0.5; wrapMode: Text.Wrap }
            }
        }
    }
    Flow {
        visible: (root.entry.synonyms || []).length > 0
        Layout.fillWidth: true; spacing: 7
        Repeater {
            model: root.entry.synonyms || []
            delegate: Rectangle {
                required property string modelData
                width: synonym.implicitWidth + 18; height: 27; radius: 6; color: Theme.subtle; border.color: Theme.border
                UiText { id: synonym; text: modelData; anchors.centerIn: parent; color: Theme.secondary; font.family: "Segoe UI"; font.pixelSize: 12 }
            }
        }
    }
}
