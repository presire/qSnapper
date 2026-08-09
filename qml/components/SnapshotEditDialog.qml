// 既存スナップショットのメタデータ (description / cleanup / userdata) を編集するダイアログ
// YaST snapper の ModifySnapshotPopup 相当
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property var snapshotListModel: null
    property int snapshotNumber: 0
    property string descriptionInitial: ""
    property string cleanupInitial: ""
    property var userdataInitial: ({})

    title: qsTr("Modify Snapshot #%1").arg(snapshotNumber)
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel
    width: {
        if (!ApplicationWindow.window) return 600
        var calculated = ApplicationWindow.window.width * 0.5
        return Math.min(Math.max(calculated, 600), 1200)
    }
    height: {
        if (!ApplicationWindow.window) return 500
        var calculated = ApplicationWindow.window.height * 0.55
        return Math.min(Math.max(calculated, 500), 800)
    }

    // ダイアログが開かれるたびに初期値を反映
    onOpened: {
        descField.text = descriptionInitial
        cleanupCombo.editText = cleanupInitial
        // userdataInitial (QVariantMap) を「key=value」行形式に変換
        var lines = []
        var keys = Object.keys(userdataInitial || {})
        for (var i = 0; i < keys.length; i++) {
            lines.push(keys[i] + "=" + userdataInitial[keys[i]])
        }
        userdataField.text = lines.join("\n")
    }

    ColumnLayout {
        width: parent.width
        spacing: 15
        anchors.top: parent.top
        anchors.topMargin: 15

        Label { text: qsTr("Description:") }
        TextField {
            id: descField
            Layout.fillWidth: true
            placeholderText: qsTr("Enter snapshot description")
        }

        Label { text: qsTr("Cleanup algorithm:") }
        // YaST 同様に編集可能 ComboBox (任意文字列も許可)
        ComboBox {
            id: cleanupCombo
            Layout.fillWidth: true
            editable: true
            model: ["", "number", "timeline", "empty-pre-post"]
        }

        Label { text: qsTr("User data (key=value per line):") }
        // ユーザデータ入力欄 (key=value を改行区切りで複数行入力)
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            color: palette.base
            border.color: userdataField.activeFocus
                ? palette.highlight
                : Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.35)
            border.width: userdataField.activeFocus ? 2 : 1
            radius: 3

            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                clip: true

                TextArea {
                    id: userdataField
                    placeholderText: qsTr("e.g.\nimportant=yes\nreason=manual")
                    wrapMode: TextArea.NoWrap
                    color: palette.text
                    placeholderTextColor: palette.placeholderText
                    background: null
                }
            }
        }
    }

    // userdata テキストを { k: v } に変換
    function parseUserdata(text) {
        var result = ({})
        var lines = text.split(/[\n,]/)
        for (var i = 0; i < lines.length; i++) {
            var line = lines[i].trim()
            if (line.length === 0) continue
            var eq = line.indexOf("=")
            if (eq <= 0) continue
            var key = line.substring(0, eq).trim()
            var val = line.substring(eq + 1).trim()
            if (key.length > 0) result[key] = val
        }
        return result
    }

    onAccepted: {
        if (!snapshotListModel) return
        var ud = parseUserdata(userdataField.text)
        snapshotListModel.modifySnapshot(snapshotNumber,
                                         descField.text,
                                         cleanupCombo.editText,
                                         ud)
    }
}
