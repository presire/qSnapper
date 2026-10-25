/**
 * @file SnapshotEditDialog.qml
 * @brief 既存スナップショットのメタデータを編集するダイアログ
 *
 * description / cleanup / userdataを編集し、[OK]でSnapshotListModel経由でSnapperへ反映する
 * YaST snapperのModifySnapshotPopup相当
 *
 * ユーザデータはkey=value形式で1行に1件入力する
 * 値は入力どおりに保存し (前後の空白も削らない)、キーは前後の空白だけを取り除く
 * 同じキーが複数あるなど、形式が不正な場合は警告を表示して[OK]を無効にする (黙って上書きしない)
 * cleanupアルゴリズムは、D-Busサービスが受け付ける名前 (空 / number / timeline / empty-pre-post) から選ぶ
 * 未知の名前が付いたスナップショットは、別の名前を選ぶまで[OK]を無効にする (黙って空へ変えない)
 *
 * 入力 : snapshotListModel (反映先モデル)、snapshotNumber、descriptionInitial / cleanupInitial / userdataInitial (初期値)
 * 出力 : modifySubmitted() (編集内容の反映を依頼した通知)
 *        成功/失敗はSnapshotListModelのsnapshotModified / snapshotModificationFailedで通知される
 * 依存 : BorderedDialog (基底)、SnapshotListModel::modifySnapshot (変更反映)、
 *        SnapshotListModel::parseUserdata (ユーザデータの解析と検証)、親から渡されるエラー色
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property var snapshotListModel: null         // 変更を反映するSnapshotListModelへの参照
    property int snapshotNumber: 0               // 編集対象のスナップショット番号
    property string descriptionInitial: ""       // 説明の初期値
    property string cleanupInitial: ""           // クリーンアップアルゴリズムの初期値
    property var userdataInitial: ({})           // ユーザデータの初期値 (QVariantMap)
    property color errorTextColor: palette.text  // 検証エラーの文言の色 (親がThemeManagerの色を渡す)

    // 入力中のユーザデータの検証結果 (SnapshotListModel::parseUserdataの戻り値: ok / userdata / error)
    // 入力のたびに評価し直し、不正な間は警告の表示と[OK]の無効化に使う
    // モデル未設定の間はC++を呼べないため、検証済みの空のユーザデータとして扱う
    readonly property var userdataCheck: root.snapshotListModel
                                         ? root.snapshotListModel.parseUserdata(userdataField.text)
                                         : ({ ok: true, userdata: ({}), error: "" })

    // 現在選んでいるcleanupアルゴリズムが、D-Busサービスの許可リストに含まれるか
    // 未知の名前はサービス側で拒否されるため、[OK]を押す前に分かるよう確定を止める
    readonly property bool cleanupSelectionValid: cleanupCombo.currentIndex >= 0  // cleanupの選択が有効か

    signal modifySubmitted()  // 編集内容の反映を依頼したことを通知する (親が処理中の表示を開く)

    title: qsTr("Modify Snapshot #%1").arg(snapshotNumber)  // タイトルに対象番号を表示
    modal: true                                             // 背後への操作をブロックする
    standardButtons: Dialog.Ok | Dialog.Cancel              // [OK]確定と[Cancel]取消を表示する
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

        // 許可リストにない名前は選択なし (-1) にし、利用者が選び直すまで確定させない
        cleanupCombo.currentIndex = cleanupCombo.indexOfValue(cleanupInitial)
        // userdataInitial (QVariantMap) を「key=value」行形式に変換する
        var lines = []
        var keys = Object.keys(userdataInitial || {})
        for (var i = 0; i < keys.length; i++) {
            lines.push(keys[i] + "=" + userdataInitial[keys[i]])
        }
        userdataField.text = lines.join("\n")
    }

    // 形式が不正なユーザデータや未知のcleanupのまま確定できないよう、[OK]を検証結果に連動させる
    // standardButtonはフッターのDialogButtonBoxが作られた後でしか取得できないため、構築完了時に結び付ける
    Component.onCompleted: {
        root.standardButton(Dialog.Ok).enabled = Qt.binding(function() {
            return root.userdataCheck.ok === true && root.cleanupSelectionValid
        })
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

        // D-Busサービスが受け付ける名前だけを選べるComboBox
        // 未知の名前を保存するとcleanupの対象から黙って外れるため、任意の文字列は入力させない
        ComboBox {
            id: cleanupCombo  // cleanupアルゴリズムの選択欄の参照名

            Layout.fillWidth: true                               // 幅いっぱいに広げる
            model: ["", "number", "timeline", "empty-pre-post"]  // サービスが受け付ける名前 (空文字列はcleanupなし)
        }

        // 既存の未知のcleanupアルゴリズムに対する警告
        // 許可リストの名前を選んでいる間は不要なため表示しない
        Label {
            visible: !root.cleanupSelectionValid  // 選択なし (-1) のときだけ表示する
            text: qsTr("The current cleanup algorithm \"%1\" is not supported. Select one from the list.").arg(root.cleanupInitial)
            textFormat: Text.PlainText            // スナップショット由来の名前を埋め込むため、リッチテキストとして解釈させない
            color: root.errorTextColor            // 親から渡された警告色
            wrapMode: Text.WordWrap               // 幅に収まるよう折り返す
            Layout.fillWidth: true                // 折り返しの基準となる幅を確保する
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

        // ユーザデータの形式が不正な場合の警告
        // 重複したキーを黙って上書きすると値が変わってしまうため、原因を示して修正を促す
        Label {
            visible: root.userdataCheck.ok !== true  // 検証に失敗している間だけ表示する
            text: root.userdataCheck.error           // SnapshotListModel::parseUserdataが返したエラー文言
            textFormat: Text.PlainText               // 入力されたキー名を含み得るため、リッチテキストとして解釈させない
            color: root.errorTextColor               // 親から渡された警告色
            wrapMode: Text.WordWrap                  // 幅に収まるよう折り返す
            Layout.fillWidth: true                   // 折り返しの基準となる幅を確保する
        }
    }

    // [OK]押下時にSnapshotListModel::modifySnapshotへ編集内容を渡して反映する
    // 反映はD-Busを非同期に呼び出すため、先にmodifySubmittedで親へ通知し、処理中の表示を開いてもらう
    // 検証に失敗している場合は[OK]が無効のため通常は到達しないが、念のため再検証して拒否する
    onAccepted: {
        if (!snapshotListModel) {
            return
        }

        if (root.userdataCheck.ok !== true) {
            return
        }

        if (!root.cleanupSelectionValid) {
            return
        }

        root.modifySubmitted()
        snapshotListModel.modifySnapshot(snapshotNumber,
                                         descField.text,
                                         cleanupCombo.currentText,
                                         root.userdataCheck.userdata)
    }
}
