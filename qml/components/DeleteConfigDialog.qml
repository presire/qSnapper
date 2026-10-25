/**
 * @file DeleteConfigDialog.qml
 * @brief snapper設定とその全スナップショットを削除する確認ダイアログ
 *
 * SettingsPageのヘッダから開く
 * 設定名を正確に入力させ、破壊的な操作を誤って実行しないようにする
 * サブボリュームがルートの設定はサーバ側が削除を拒否するため、[OK]を無効にして理由を示す
 *
 * 入力 : manager (ConfigManagerModelへの参照)、configName (削除対象の設定名)、subvolume (対象のサブボリューム)
 * 出力 : なし (削除の成否はConfigManagerModelのconfigDeleted / configDeletionFailedで通知される)
 * 依存 : ConfigManagerModel::deleteConfig、ThemeManager (エラー色)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property var manager: null                  // 削除を要求するConfigManagerModelへの参照
    property string configName: ""              // 削除対象の設定名
    property string subvolume: ""               // 削除対象の設定が使うサブボリューム

    // 配色の参照
    // qmllintがシングルトンを解決できないため、var経由にして警告の増加を避ける
    readonly property var theme: ThemeManager

    readonly property bool rootConfig: subvolume === "/"                                  // ルート設定はサーバ側が削除を拒否する
    readonly property bool nameMatches: configName.length > 0 && confirmField.text === configName // 確認入力が設定名と一致するか
    readonly property bool canDelete: !rootConfig && nameMatches && manager !== null && !manager.busy // [OK]を有効にしてよいか

    title: qsTr("Delete Configuration")         // タイトルバーの表示名
    modal: true                                 // 背後への操作をブロックする
    standardButtons: Dialog.Ok | Dialog.Cancel  // [OK]で削除、[Cancel]で取消
    anchors.centerIn: Overlay.overlay           // 常にオーバーレイ中央へ配置する
    width: Math.min(560, ApplicationWindow.window ? ApplicationWindow.window.width - 80 : 560)
    height: 470

    // 開くたびに確認入力を初期化する
    // 前回の入力が残ると確認の意味がなくなるため
    onOpened: {
        confirmField.text = ""
        confirmField.forceActiveFocus()
    }

    // 設定名が一致し、ルート設定でなく、実行中でない場合だけ[OK]を有効にする
    // 全スナップショットが消える操作のため、確認入力を必須にして誤操作を防ぐ
    Component.onCompleted: {
        var okButton = standardButton(Dialog.Ok)
        if (okButton) {
            okButton.enabled = Qt.binding(function() { return root.canDelete })
            // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
            // そのため、無効状態であることは不透明度を下げて示す
            okButton.opacity = Qt.binding(function() { return okButton.enabled ? 1.0 : 0.45 })
        }
    }

    // ConfigManagerModel::deleteConfigを呼び出し、結果はconfigDeleted / configDeletionFailedで受け取る
    onAccepted: {
        if (manager) {
            manager.deleteConfig(configName)
        }
    }

    ColumnLayout {
        width: parent.width
        spacing: 12
        anchors.top: parent.top
        anchors.topMargin: 15

        // 削除対象を明示する
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            font.bold: true
            text: qsTr("Delete configuration \"%1\"?").arg(root.configName)
        }

        // 影響範囲を強調する
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: root.theme.errorColor
            text: qsTr("All snapshots that belong to this configuration will be deleted.")
        }

        // 取り消せない操作であることを示す
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("This operation cannot be undone.")
        }

        // 認証が要求されることを事前に示す
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("Administrator privileges are required.")
        }

        // ルート設定の場合は[OK]を無効にし、別の設定を作るよう案内する
        // サーバ側もルート設定の削除を拒否するため、認証を無駄に発生させない
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            visible: root.rootConfig
            color: root.theme.errorColor
            text: qsTr("The root configuration (subvolume /) cannot be deleted. Create a configuration for a subvolume other than the filesystem root.")
        }

        // 確認入力の説明
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("Type the configuration name to confirm:")
        }

        TextField {
            id: confirmField
            Layout.fillWidth: true
            placeholderText: root.configName
        }
    }
}
