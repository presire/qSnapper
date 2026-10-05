/**
 * @file SnapshotEditDialog.qml
 * @brief 既存スナップショットのメタデータを編集するダイアログ
 *
 * description / cleanup / userdataを編集し、[OK]でSnapshotListModel経由でSnapperへ反映する
 * YaST snapperのModifySnapshotPopup相当
 *
 * 入力 : snapshotListModel (反映先モデル)、snapshotNumber、descriptionInitial / cleanupInitial / userdataInitial (初期値)
 * 出力 : なし (成功/失敗はSnapshotListModelのsnapshotModified / snapshotModificationFailedで通知される)
 * 依存 : BorderedDialog (基底)、SnapshotListModel::modifySnapshot (変更反映)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property var snapshotListModel: null      // 変更を反映するSnapshotListModelへの参照
    property int snapshotNumber: 0            // 編集対象のスナップショット番号
    property string descriptionInitial: ""    // 説明の初期値
    property string cleanupInitial: ""        // クリーンアップアルゴリズムの初期値
    property var userdataInitial: ({})        // ユーザデータの初期値 (QVariantMap)

    title: qsTr("Modify Snapshot #%1").arg(snapshotNumber)  // タイトルに対象番号を表示
    modal: true                               // 背後への操作をブロックする
    standardButtons: Dialog.Ok | Dialog.Cancel  // [OK]確定と[Cancel]取消を表示する
    // ウィンドウ幅の50%を基準に、600から1200の範囲へクランプする
    width: {
        if (!ApplicationWindow.window) return 600
        var calculated = ApplicationWindow.window.width * 0.5
        return Math.min(Math.max(calculated, 600), 1200)
    }
    // ウィンドウ高さの55%を基準に、500から800の範囲へクランプする
    height: {
        if (!ApplicationWindow.window) return 500
        var calculated = ApplicationWindow.window.height * 0.55
        return Math.min(Math.max(calculated, 500), 800)
    }

    // ダイアログを開くたびに呼び出し元が渡した初期値を各入力欄へ反映する
    onOpened: {
        descField.text = descriptionInitial
        cleanupCombo.editText = cleanupInitial
        // userdataInitial (QVariantMap) を「key=value」行形式に変換する
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

        // 説明の入力欄
        Label { text: qsTr("Description:") }
        TextField {
            id: descField
            Layout.fillWidth: true
            placeholderText: qsTr("Enter snapshot description")
        }

        // クリーンアップアルゴリズムの入力欄
        Label { text: qsTr("Cleanup algorithm:") }
        // YaST同様に編集可能なComboBox (任意文字列も許可する)
        ComboBox {
            id: cleanupCombo
            Layout.fillWidth: true
            editable: true
            model: ["", "number", "timeline", "empty-pre-post"]
        }

        // ユーザデータの入力欄
        Label { text: qsTr("User data (key=value per line):") }
        // key=valueを改行区切りで複数行入力するテキストエリア
        // フォーカス時は枠線を強調色に切り替える
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

                // ユーザデータを編集する本体 (枠は親Rectangleが描くため背景を消す)
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

    /**
     * @brief userdataテキストをkey-valueマップへ変換する
     * @param text 改行またはカンマ区切りの「key=value」文字列
     * @return 解析結果のマップ (不正な行は無視する)
     * @note 副作用なし (純粋な変換のみ)
     */
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

    // [OK]押下時にSnapshotListModel::modifySnapshotへ編集内容を渡して反映する
    onAccepted: {
        if (!snapshotListModel) return
        var ud = parseUserdata(userdataField.text)
        snapshotListModel.modifySnapshot(snapshotNumber,
                                         descField.text,
                                         cleanupCombo.editText,
                                         ud)
    }
}
