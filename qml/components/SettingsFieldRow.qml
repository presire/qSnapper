/**
 * @file SettingsFieldRow.qml
 * @brief snapper設定の1つのキーを編集する行
 *
 * 値の種別 (boolean / seconds / countOrRange / fraction) に応じて、スイッチまたはテキスト入力を表示する
 * 入力値は設定ファイル由来で信頼できないため、すべてプレーンテキストとして扱う
 * SettingsPageのグループ内に並べて使用する
 *
 * 入力 : settings (編集対象のSnapperSettingsModel)、settingKey (設定キー)、label (表示名)
 * 出力 : なし (編集内容はsettingsへ直接反映する)
 * 依存 : SnapperSettingsModel (値の取得 / 編集 / 検証)、ThemeManager (エラー色)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

RowLayout {
    id: root

    required property var settings                      // 編集対象のSnapperSettingsModel
    required property string settingKey                 // 設定キー (例: NUMBER_LIMIT)
    required property string label                      // 画面に表示する項目名

    readonly property string kind: settings.valueKind(settingKey)   // 値の種別
    // revisionを参照して、読み込みと編集のたびにバインディングを再評価させる
    readonly property string currentValue: {
        settings.revision
        return settings.value(settingKey)
    }
    readonly property bool changed: {
        settings.revision
        return settings.isModified(settingKey)
    }
    readonly property bool valid: {
        settings.revision
        return settings.isValidValue(settingKey, settings.value(settingKey))
    }
    // 読み込み前と読み込み中は編集させない (比較対象の読み込み値が無く、直後の読み込みで上書きされるため)
    readonly property bool editable: settings.configLoaded && !settings.configBusy

    // 種別ごとの入力形式の説明
    readonly property string hint: {
        switch (kind) {
        case "seconds":
            return qsTr("Seconds (0 to 999999999)")
        case "countOrRange":
            return qsTr("Number or MIN-MAX (for example 10 or 5-10)")
        case "fraction":
            return qsTr("0 to 1 (for example 0.5)")
        default:
            return ""
        }
    }

    spacing: 10

    // 項目名 (未保存の変更がある項目は太字にして、保存対象を判別できるようにする)
    Label {
        text: root.label
        textFormat: Text.PlainText
        font.bold: root.changed
        Layout.preferredWidth: 280
        elide: Text.ElideRight
    }

    // yes / noの設定: 状態が一目で分かるようSwitchで編集する
    Switch {
        visible: root.kind === "boolean"
        enabled: root.editable
        checked: root.currentValue === "yes"
        text: checked ? qsTr("Yes") : qsTr("No")
        onToggled: root.settings.setValue(root.settingKey, checked ? "yes" : "no")
    }

    // 数値と割合の設定: 不正な値も入力途中として保持し、赤枠と説明で知らせる
    TextField {
        visible: root.kind !== "boolean"
        enabled: root.editable
        text: root.currentValue
        selectByMouse: true
        Layout.preferredWidth: 160
        onTextEdited: root.settings.setValue(root.settingKey, text)

        background: Rectangle {
            color: parent.palette.base
            border.width: parent.activeFocus ? 2 : 1
            border.color: !root.valid && root.editable
                ? ThemeManager.errorColor
                : (parent.activeFocus ? parent.palette.highlight : parent.palette.mid)
            radius: 3
        }
    }

    // 入力形式の説明 (不正な値のときはエラー色で強調する)
    Label {
        visible: root.kind !== "boolean"
        text: root.hint
        textFormat: Text.PlainText
        color: !root.valid && root.editable ? ThemeManager.errorColor : palette.placeholderText
        font.pixelSize: 12
        Layout.fillWidth: true
        elide: Text.ElideRight
    }

    Item {
        visible: root.kind === "boolean"
        Layout.fillWidth: true
    }
}
