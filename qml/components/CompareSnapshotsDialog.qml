/**
 * @file CompareSnapshotsDialog.qml
 * @brief 任意の2スナップショット間のファイル差分を表示するダイアログ
 *
 * YaST snapperのcreate_comparison + get_files相当
 * FileChangeModelで2つの番号間の差分を取得し、フラットな一覧として表示する
 *
 * 差分の取得に失敗した場合は、差分が無い正常な状態と区別できるよう、エラー文言をダイアログ内に表示する
 *
 * 入力 : configName (Snapper設定名)、num1 / num2 (比較するスナップショット番号)、
 *        errorTextColor (エラー文言の色、親がThemeManagerの色を渡す)
 * 出力 : なし
 * 依存 : BorderedDialog (基底)、FileChangeModel (loadChangesBetween / loaded / loading / hasChanges / errorOccurred)、
 *        ThemeManager (パレット)、DisplayText (双方向制御文字の置き換え)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property string configName: "root"              // 比較対象のSnapper設定名
    property int num1: 0                            // 比較元スナップショット番号
    property int num2: 0                            // 比較先スナップショット番号
    property color errorTextColor: palette.text     // 取得エラーの文言の色 (親がThemeManagerの色を渡す)
    property string errorMessage: ""                // 直近の読み込みで起きたエラーの文言 (空文字列はエラーなし)
    readonly property var compareRef: compareModel  // FileChangeModelへの参照 (loadedとerrorOccurredの参照用)

    title: qsTr("Compare Snapshot #%1 ↔ #%2").arg(num1).arg(num2)  // タイトルに対象の2番号を表示
    modal: true                                                    // 背後への操作をブロックする
    standardButtons: Dialog.Close                                  // 閉じるボタンのみ表示する
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

    // compareRefは、qmllintがC++側で登録された型のメンバを解決できず警告が増えるため、var経由でFileChangeModelを参照する

    // 差分を保持するモデル (configNameに追従して対象設定を切り替える)
    FileChangeModel {
        id: compareModel
        configName: root.configName
    }

    // FileChangeModelの読み込み失敗をダイアログ内に表示するための接続
    // 差分が無い場合はエラーにならないため、取得に失敗したときだけエラー文言を保持する
    Connections {
        target: root.compareRef

        // FileChangeModel::errorOccurredで受け取った文言を保持し、下部のエラー表示ラベルに出す
        function onErrorOccurred(message) {
            root.errorMessage = message
        }
    }

    // 表示開始時に2番号間の差分を読み込む
    // 番号が未設定 (0) の場合は読み込みを行わない
    // 前回の失敗文言が新しい読み込みの結果として残らないよう、先にエラー文言を消去する
    onOpened: {
        root.errorMessage = ""

        if (num1 > 0 && num2 > 0) {
            compareModel.loadChangesBetween(num1, num2, true)   // flat=trueでListView向けの一覧を構築する
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // 読み込み状態に応じて見出しを切り替える
        Label {
            textFormat: Text.PlainText  // 番号を埋め込む文言だが、他の表示と揃えてリッチテキスト解釈を無効にする
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
                        textFormat: Text.PlainText  // 状態フラグを書式解釈させない
                        text: model.statusFlags || ""
                        font.family: "monospace"
                        Layout.preferredWidth: 60
                    }
                    // 変更されたファイルのパスを表示する
                    Label {
                        textFormat: Text.PlainText  // ファイル名にタグ状の文字列が含まれていても、リッチテキストとして解釈させない
                        text: DisplayText.sanitize(model.filePath || "")
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                    }
                }
            }
            ScrollBar.vertical: ScrollBar {}

            // 読み込みに成功し、差分が無い場合のプレースホルダ (差分が無いのはエラーではなく正常な状態)
            // FileChangeModel::loadedは0件の成功でもtrueになるため、読み込み前や失敗時には表示しない
            Label {
                anchors.centerIn: parent
                visible: !compareModel.loading && root.compareRef.loaded && !compareModel.hasChanges
                text: qsTr("No differences found")
                color: palette.placeholderText
            }
        }

        // 差分の取得に失敗したときのエラー表示
        // 差分が無い場合のプレースホルダと区別するため、エラー文言があるときだけ表示する
        Label {
            visible: root.errorMessage !== ""
            text: root.errorMessage
            textFormat: Text.PlainText  // C++側のエラー文言にパスなどが含まれるため、リッチテキストとして解釈させない
            color: root.errorTextColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
