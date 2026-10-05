/**
 * @file CompareSnapshotsDialog.qml
 * @brief 任意の2スナップショット間のファイル差分を表示するダイアログ
 *
 * YaST snapperのcreate_comparison + get_files相当
 * FileChangeModelで2つの番号間の差分を取得し、フラットな一覧として表示する
 *
 * 入力 : configName (Snapper設定名)、num1 / num2 (比較するスナップショット番号)
 * 出力 : なし
 * 依存 : BorderedDialog (基底)、FileChangeModel::loadChangesBetween (差分取得)、ThemeManager (パレット)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property string configName: "root"             // 比較対象のSnapper設定名
    property int num1: 0                           // 比較元スナップショット番号
    property int num2: 0                           // 比較先スナップショット番号

    title: qsTr("Compare Snapshot #%1 ↔ #%2").arg(num1).arg(num2)  // タイトルに対象の2番号を表示
    modal: true                                    // 背後への操作をブロックする
    standardButtons: Dialog.Close                  // 閉じるボタンのみ表示する
    // ウィンドウ幅の75%を基準に、900から1600の範囲へクランプする
    width: {
        if (!ApplicationWindow.window) return 900
        var calculated = ApplicationWindow.window.width * 0.75
        return Math.min(Math.max(calculated, 900), 1600)
    }
    // ウィンドウ高さの75%を基準に、600から1000の範囲へクランプする
    height: {
        if (!ApplicationWindow.window) return 600
        var calculated = ApplicationWindow.window.height * 0.75
        return Math.min(Math.max(calculated, 600), 1000)
    }

    // 差分を保持するモデル (configNameに追従して対象設定を切り替える)
    FileChangeModel {
        id: compareModel
        configName: root.configName
    }

    // 表示開始時に2番号間の差分を読み込む
    // 番号が未設定 (0) の場合は読み込みを行わない
    onOpened: {
        if (num1 > 0 && num2 > 0) {
            compareModel.loadChangesBetween(num1, num2, true)   // flat=trueでListView向けの一覧を構築する
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // 読み込み状態に応じて見出しを切り替える
        Label {
            text: compareModel.loading
                  ? qsTr("Loading comparison...")
                  : qsTr("Files that differ between snapshot #%1 and #%2").arg(num1).arg(num2)
            font.bold: true
        }

        // 差分ファイル一覧 (ツリー表示はフラットに省略)
        ListView {
            id: fileList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: compareModel
            // 1ファイルを状態フラグとパスで表示する行
            delegate: ItemDelegate {
                width: fileList.width
                height: 28
                contentItem: RowLayout {
                    spacing: 10
                    // 変更種別フラグ (+/-/c等) を等幅フォントで表示する
                    Label {
                        text: model.statusFlags || ""
                        font.family: "monospace"
                        Layout.preferredWidth: 60
                    }
                    // 変更されたファイルのパスを表示する
                    Label {
                        text: model.filePath || ""
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                    }
                }
            }
            ScrollBar.vertical: ScrollBar {}

            // 差分が無く読み込みも完了している場合のプレースホルダ
            Label {
                anchors.centerIn: parent
                visible: !compareModel.loading && !compareModel.hasChanges
                text: qsTr("No differences found")
                color: palette.placeholderText
            }
        }
    }
}
