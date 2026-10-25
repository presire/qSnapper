/**
 * @file CreateConfigDialog.qml
 * @brief snapper設定を新規作成するダイアログ
 *
 * SettingsPageのヘッダから開く
 * fstypeはサーバ側でbtrfs、テンプレートはdefaultに固定されるため、名前とサブボリュームだけを入力する
 * 認証はサーバ側のpolkitが行うため、[Create]を押すとダイアログを閉じ、結果はSettingsPageのフッターへ表示する
 *
 * 入力 : manager (ConfigManagerModelへの参照)
 * 出力 : なし (作成の成否はConfigManagerModelのconfigCreated / configCreationFailedで通知される)
 * 依存 : ConfigManagerModel::isValidConfigName / isValidSubvolumePath / createConfig、ThemeManager (エラー色)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property var manager: null                  // 作成を要求するConfigManagerModelへの参照

    // 配色の参照
    // qmllintがシングルトンを解決できないため、var経由にして警告の増加を避ける
    readonly property var theme: ThemeManager

    readonly property bool nameValid: manager ? manager.isValidConfigName(nameField.text) : false          // 設定名が規則に適合するか
    readonly property bool subvolumeValid: manager ? manager.isValidSubvolumePath(subvolumeField.text) : false // パスが規則に適合するか
    readonly property bool canSubmit: nameValid && subvolumeValid && manager !== null && !manager.busy     // [Create]を有効にしてよいか

    title: qsTr("Create Configuration")         // タイトルバーの表示名
    modal: true                                 // 背後への操作をブロックする
    standardButtons: Dialog.Ok | Dialog.Cancel  // [OK]で作成、[Cancel]で取消
    anchors.centerIn: Overlay.overlay           // 常にオーバーレイ中央へ配置する
    width: Math.min(620, ApplicationWindow.window ? ApplicationWindow.window.width - 80 : 620)
    height: 430

    // 開くたびに入力と検証結果を初期化する
    // 前回の入力が残ると別の設定を誤って作成しかねないため
    onOpened: {
        nameField.text = ""
        subvolumeField.text = ""
        nameField.forceActiveFocus()
    }

    // 入力がそろい、実行中でない場合だけ[OK]を有効にする
    // 不正な値はサーバ側でも拒否されるため、認証ダイアログを無駄に表示しないよう送信前に止める
    Component.onCompleted: {
        var okButton = standardButton(Dialog.Ok)
        if (okButton) {
            okButton.enabled = Qt.binding(function() { return root.canSubmit })
            // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
            // そのため、無効状態であることは不透明度を下げて示す
            okButton.opacity = Qt.binding(function() { return okButton.enabled ? 1.0 : 0.45 })
        }
    }

    // ConfigManagerModel::createConfigを呼び出し、結果はconfigCreated / configCreationFailedで受け取る
    onAccepted: {
        if (manager) {
            manager.createConfig(nameField.text, subvolumeField.text)
        }
    }

    ColumnLayout {
        width: parent.width
        spacing: 12
        anchors.top: parent.top
        anchors.topMargin: 15

        // 作成される設定の内容と前提条件を示す
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("A new snapper configuration is created with the btrfs filesystem type and the default template. A .snapshots subvolume is created automatically beneath the given subvolume. The subvolume must already be mounted and must not be the filesystem root.")
        }

        // 設定名の入力欄
        Label { text: qsTr("Name:") }
        TextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("Enter a configuration name")
        }

        // 入力途中は規則違反をその場で示し、何を直せばよいか分かるようにする
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: nameField.text.length > 0 && !root.nameValid
            color: root.theme.errorColor
            text: qsTr("The name must start with a letter or digit and may contain only letters, digits, _ and - (up to 64 characters).")
        }

        // サブボリュームの入力欄
        Label { text: qsTr("Subvolume:") }
        TextField {
            id: subvolumeField
            Layout.fillWidth: true
            placeholderText: qsTr("/path/to/btrfs/subvolume")
        }

        // 入力途中は規則違反をその場で示し、認証前に誤りを気付けるようにする
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: subvolumeField.text.length > 0 && !root.subvolumeValid
            color: root.theme.errorColor
            text: qsTr("Enter an absolute path of a mounted Btrfs subvolume. The path must not contain //, . or .. components, must not end with /, and must not be /.")
        }
    }
}
