/**
 * @file RestorePreviewDialog.qml
 * @brief スナップショットとの差分を確認し、選択したファイルを復元するダイアログ
 *
 * SnapshotDetailDialogから開く
 * 復元はFileChangeModelがまとめて行うため、Polkitの認証は1回で済む
 * Pre/Postペアでは、Pre <--> Post間の差分ビューからだけ復元できる (「vs 現在」ビューは閲覧専用)
 *
 * 復元の確認では、live側から削除されるパスの件数と一覧を必ず見せる
 * 復元後は、適用・削除・スキップの件数と、失敗した位置を結果として表示する
 *
 * 入力 : configName、snapshotNumber、preSnapshotNumber、postSnapshotNumber (親からバインド)
 * 出力 : restoreConfirmed()
 * 依存 : ThemeManager (配色)、FileChangeModel (差分ツリーと復元、削除対象の一覧、復元結果)、
 *        BorderedDialog (枠付きダイアログ)、ThemedGroupBox (枠付きグループ)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property string configName: "root"                                                     // Snapper設定名
    property int snapshotNumber: 0                                                         // 開いたときに表示するスナップショット番号
    property int preSnapshotNumber: 0                                                      // Preスナップショット番号 (0 = Pre/Postペアではない)
    property int postSnapshotNumber: 0                                                     // Postスナップショット番号
    // snapshotNumberは親からバインドされており、書き換えるとバインドが外れるため、表示の切り替えにはこちらを使う
    property int activeSnapshotNumber: 0                                                   // 現在表示中のスナップショット番号
    readonly property bool isPrePostPair: preSnapshotNumber > 0 && postSnapshotNumber > 0  // Pre/Postペアかどうか

    // 表示モード
    // 復元の向きはラジオボタンを選んだ時点で決める (復元時に読み直すと、チェックした内容が消えるため)
    readonly property int viewRevertToPre: 0     // Pre <--> Post差分をPreへ戻す (復元可)
    readonly property int viewReapplyToPost: 1   // Pre <--> Post差分をPostへ再適用する (復元可)
    readonly property int viewVsCurrent: 2       // スナップショットと現在のシステムの差分 (Pre/Postペアでは閲覧専用)
    property int viewMode: root.viewVsCurrent    // 現在の表示モード

    readonly property bool prePostDiffMode: root.viewMode === root.viewRevertToPre  // Pre <--> Post差分を表示中かどうか
                                            || root.viewMode === root.viewReapplyToPost

    // Pre/Postペアの「vs 現在」ビューから復元すると、Post以降に手動で変えた内容まで戻るため閲覧専用にする
    readonly property bool restorable: root.isPrePostPair ? root.prePostDiffMode : true  // 現在の表示から復元できるかどうか

    readonly property int restoreSourceNumber: {  // 復元元のスナップショット番号 (0 = 復元できない)
        if (!root.isPrePostPair) return root.activeSnapshotNumber
        if (root.viewMode === root.viewRevertToPre) return root.preSnapshotNumber
        if (root.viewMode === root.viewReapplyToPost) return root.postSnapshotNumber
        return 0
    }

    // 差分モデルへの参照 (loaded / truncated / omittedCount / restoreResult / checkedDeletePreviewの参照に使う)
    // qmllintはC++側で登録された型のメンバを解決できないため、var経由で参照して警告の増加を避ける
    readonly property var changeModel: fileChangeModel  // FileChangeModelへの参照

    // 警告やエラーの文言の色 (既存のラベルの配色を再利用し、配色の定義をThemeManagerに一本化する)
    readonly property color errorTextColor: errorLabel.color               // エラー文言の色 (ThemeManager.errorColor)
    readonly property color warningTextColor: overwriteWarningLabel.color  // 警告文言の色 (ThemeManager.warningColor)

    // 直近の復元計画の結果 (サーバのGetRestorePlanStatusを解析したもの)
    // 復元の完了通知より後に届くことがあるため、結果ダイアログはこのプロパティのバインディングで更新する
    readonly property var restoreResult: root.changeModel.restoreResult  // 適用・削除・スキップの件数と失敗位置 (未取得の間は空)

    // [選択項目を#Nへ復元]ボタンの有効状態の判定に使い、updateHasCheckedItemsだけが値を更新する
    property bool hasCheckedItems: false  // 差分ツリーで復元するファイルに1つでもチェックが入っているかどうか

    signal restoreConfirmed()                        // 復元の確定を親へ通知する

    /**
     * @brief 差分ツリーにチェックの入ったファイルがあるかどうかを調べ直し、hasCheckedItemsへ反映する
     * @note FileChangeModel::hasCheckedItemsを呼び出し、差分ツリーのチェック状態を調べる
     *       hasCheckedItemsの変更により、[選択項目を#Nへ復元]ボタンの有効状態が切り替わる
     */
    function updateHasCheckedItems() {
        root.hasCheckedItems = fileChangeModel.hasCheckedItems()
    }

    /**
     * @brief 右ペインの選択ファイルと差分表示をクリアする
     * @note FileChangeModelの状態は変更しない
     */
    function resetRightPane() {
        rightPane.fileSelected = false
        rightPane.selectedFilePath = ""
        rightPane.selectedChangeType = -1
        rightPane.selectedStatusFlags = ""
        rightPane.fileLoading = false
        rightPane.fileDetails = {}
        diffTextArea.textFormat = TextEdit.PlainText
        diffTextArea.text = ""
    }

    /**
     * @brief 指定したスナップショットと現在のシステムの差分表示へ切り替える
     * @param targetNumber 比較するスナップショット番号
     * @note FileChangeModel::loadChanges()でツリーを読み直し、右ペインもクリアする
     */
    function switchSnapshotView(targetNumber) {
        if (root.viewMode === root.viewVsCurrent && root.activeSnapshotNumber === targetNumber) return
        root.viewMode = root.viewVsCurrent
        root.activeSnapshotNumber = targetNumber
        fileChangeModel.snapshotNumber = targetNumber  // 番号を設定すると、モデルは現在のシステムとの比較に戻る
        resetRightPane()
        fileChangeModel.loadChanges()
    }

    /**
     * @brief Pre <--> Post間の差分表示へ切り替える
     * @param mode viewRevertToPre または viewReapplyToPost
     * @note Pre/Postペアでない場合と、既に同じモードの場合は何もしない
     */
    function switchPrePostDiffView(mode) {
        if (!root.isPrePostPair) return
        if (root.viewMode === mode) return
        root.viewMode = mode
        resetRightPane()
        loadPrePostDiff()
    }

    /**
     * @brief 現在のviewModeの向きでPre <--> Post差分を読み込む
     * @note FileChangeModel::loadChangesBetween()は第1引数を復元元とするため、戻したい側のスナップショットを先に渡す
     *       第3引数のfalseでツリー表示にし、第4引数のtrueで復元済みなど現在のシステムと同じファイルを除外する
     */
    function loadPrePostDiff() {
        if (root.viewMode === root.viewRevertToPre) {
            fileChangeModel.loadChangesBetween(root.preSnapshotNumber,
                                               root.postSnapshotNumber,
                                               false,
                                               true)
        }
        else {
            fileChangeModel.loadChangesBetween(root.postSnapshotNumber,
                                               root.preSnapshotNumber,
                                               false,
                                               true)
        }
    }

    /**
     * @brief 現在の表示条件のままファイルツリーを読み直す
     * @note 復元後の再表示に使う
     */
    function reloadCurrentView() {
        if (root.prePostDiffMode) {
            loadPrePostDiff()
        }
        else {
            fileChangeModel.snapshotNumber = root.activeSnapshotNumber
            fileChangeModel.loadChanges()
        }
    }

    // 復元結果の件数と失敗位置を表示する部品 (成功ダイアログと失敗ダイアログで共有する)
    // 外部のidを参照できないため、結果はresultプロパティで受け取る
    // パスとエラー文言は復元対象に由来するため、全てPlainTextで表示する
    component RestoreResultSummary: ColumnLayout {
        id: summary

        required property var result                                 // 復元結果 (restoreResultと同じ形式)
        required property color errorTextColor                       // 失敗位置の文言の色 (外部のidを参照できないため、親から受け取る)
        readonly property bool valid: summary.result.valid === true  // 結果を取得できているかどうか

        // 一覧に載っているスキップ済みパスの件数 (サーバは先頭100件までしか返さないため、件数の合計とは一致しないことがある)
        readonly property int skippedListed: summary.valid && summary.result.skippedPaths ? summary.result.skippedPaths.length : 0

        // 結果を取得できるまでは何も表示しない
        visible: summary.valid
        spacing: 6

        // 適用できたエントリ数と全体のエントリ数
        Label {
            text: qsTr("Applied: %1 of %2 entries").arg(summary.result.appliedCount).arg(summary.result.totalEntries)
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // 削除の件数は、追加フィールドを返す新しいサーバだけが報告する
        Label {
            visible: summary.valid && summary.result.hasExtended === true
            text: qsTr("Deleted from the current system: %1").arg(summary.result.deletedCount)
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // FIFO・ソケット・デバイスファイルは復元できないためスキップされる
        Label {
            visible: summary.valid && summary.result.skippedCount > 0
            text: qsTr("Skipped: %1 (FIFO, socket and device files cannot be restored)").arg(summary.result.skippedCount)
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // スキップしたパスの一覧 (サーバは先頭100件までしか返さない)
        ScrollView {
            visible: summary.skippedListed > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 90
            clip: true

            // スキップしたパスは復元対象に由来するため、PlainTextの読み取り専用テキストで表示する
            TextArea {
                readOnly: true
                text: summary.skippedListed > 0 ? summary.result.skippedPaths.join("\n") : ""
                textFormat: TextEdit.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                wrapMode: TextEdit.NoWrap
                font.family: "monospace"
                font.pixelSize: 11
            }
        }

        // 一覧がサーバの返却上限で打ち切られている場合だけ、残りが見えていないことを示す
        Label {
            visible: summary.valid && summary.result.skippedCount > summary.skippedListed && summary.skippedListed > 0
            text: qsTr("Only the first %1 skipped paths are listed.").arg(summary.skippedListed)
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // 失敗したエントリの位置とパス (失敗が無ければ表示しない)
        Label {
            visible: summary.valid && summary.result.failedIndex >= 0
            text: qsTr("Failed at entry #%1 of %2:").arg(summary.result.failedIndex + 1).arg(summary.result.totalEntries)
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            font.bold: true
            color: summary.errorTextColor
        }

        // 失敗したエントリのパス (サーバがパスを返した場合だけ表示する)
        Label {
            visible: summary.valid && summary.result.failedIndex >= 0 && summary.result.failedPath !== ""
            text: summary.result.failedPath
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
            font.family: "monospace"
            font.pixelSize: 11
        }

        // 最後に記録されたエラーの文言 (サーバが返した場合だけ表示する)
        Label {
            visible: summary.valid && summary.result.lastError !== ""
            text: summary.result.lastError
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    width: {
        if (!ApplicationWindow.window) return 960
        return Math.max(ApplicationWindow.window.width - 50, 960)
    }
    height: {
        if (!ApplicationWindow.window) return 720
        return Math.max(ApplicationWindow.window.height - 100, 720)
    }
    modal: true
    title: qsTr("Snapshot Overview")
    anchors.centerIn: Overlay.overlay

    // 開くたびに表示を初期化する
    onOpened: {
        console.log("RestorePreviewDialog opened with configName:", configName, "snapshotNumber:", snapshotNumber)
        errorLabel.visible = false
        fileChangeModel.configName = configName

        // 前回開いたときの選択ファイルと差分を残さない
        resetRightPane()

        // 表示するスナップショットを、親から渡された番号に戻す
        root.activeSnapshotNumber = snapshotNumber

        if (root.isPrePostPair) {
            // Pre/Postペアでは、まずPreへ戻す向きの差分を表示する
            root.viewMode = root.viewRevertToPre
            loadPrePostDiff()
        }
        else {
            root.viewMode = root.viewVsCurrent
            fileChangeModel.snapshotNumber = snapshotNumber
            fileChangeModel.loadChanges()
        }
    }

    // 次に別のスナップショットで開いたとき、前回の差分が残らないようにする
    onClosed: {
        resetRightPane()
    }

    // 差分ツリー、ファイルごとの差分の取得、復元を担うC++のモデル
    FileChangeModel {
        id: fileChangeModel

        // チェックを変更したときと差分ツリーを読み直したときに、復元ボタンの有効状態を更新する
        // フォルダにチェックを入れると、配下の項目の数だけdataChangedシグナルが届く
        // 通知のたびに調べ直すと無駄が多いため、Qt.callLaterで後回しにし、調べ直しを1回にまとめる
        onDataChanged: Qt.callLater(root.updateHasCheckedItems)
        onModelReset: Qt.callLater(root.updateHasCheckedItems)

        // エラーはダイアログ下部のラベルに表示する
        // errorLabelはPlainTextで表示するため、サーバや差分に由来する文字列がリッチテキストとして解釈されない
        onErrorOccurred: function(message) {
            errorLabel.text = message
            errorLabel.visible = true
        }

        // getFileDiffAndDetails()の応答 (選択したファイルの差分と詳細)
        onFileDiffAndDetailsReady: function(filePath, details, diff) {
            // 応答を待つ間に別のファイルが選ばれた場合は、古い応答を捨てる
            if (filePath !== rightPane.selectedFilePath) return
            rightPane.fileLoading = false
            rightPane.fileDetails = details

            // サービスが差分の生成を省略した場合は、その理由を表示する
            var omittedReason = details["diffOmitted"] || ""
            if (omittedReason !== "") {
                diffTextArea.textFormat = TextEdit.PlainText
                diffTextArea.text = rightPane.diffOmittedMessage(omittedReason)
            }
            else if (diff === "") {
                diffTextArea.textFormat = TextEdit.PlainText
                diffTextArea.text = ""
            }
            else {
                diffTextArea.textFormat = TextEdit.RichText
                diffTextArea.text = rightPane.formatDiffHtml(diff)
            }
        }

        // 復元の進捗 (1ファイルごとに届く)
        // 1件ごとに画面を更新すると描画が止まるため、ここでは貯めるだけにする
        onRestoreProgress: function(current, total, filePath) {
            progressDialog.queueProgress(current, total, filePath)
        }

        // 復元の完了
        onRestoreCompleted: function(success) {
            progressDialog.close()
            if (success) {
                root.resetRightPane()

                // 復元結果が分かるよう、現在のシステムとの差分を表示し直す
                // Pre <--> Post差分はスナップショット同士の比較で復元しても変わらないため、復元先と現在の比較へ切り替える
                var restoreTarget = root.restoreSourceNumber
                if (root.isPrePostPair && restoreTarget > 0) {
                    root.switchSnapshotView(restoreTarget)
                }
                else {
                    root.reloadCurrentView()
                }

                successDialog.open()
            }
            else {
                // 認可の拒否など、復元を始められなかった理由を失敗ダイアログにも表示する
                // 直前のerrorOccurredで設定された文言を引き継ぐ (errorLabelは復元の開始時に消している)
                restoreFailDialog.errorMessage = errorLabel.visible ? errorLabel.text : ""
                restoreFailDialog.open()
            }
        }
    }

    // 読み込み中の表示 (読み込みが終わるまでメインレイアウトの代わりに表示する)
    Item {
        anchors.fill: parent
        visible: fileChangeModel.loading

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 15

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: fileChangeModel.loading
                Layout.preferredWidth: 48
                Layout.preferredHeight: 48
            }

            Label {
                text: qsTr("Loading file changes. Please wait...")
                font.pixelSize: 14
                Layout.alignment: Qt.AlignHCenter
                color: palette.text
            }
        }
    }

    // メインレイアウト (読み込み中は操作できないよう非表示にする)
    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        visible: !fileChangeModel.loading

        // 対象のSnapper設定 (root以外の設定でも取り違えないように表示する)
        Label {
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            text: qsTr("Snapper configuration: %1").arg(root.configName)
            font.bold: true
        }

        // 現在の表示内容の説明
        Label {
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            text: root.prePostDiffMode
                  ? qsTr("Shows only the changes made between the Pre and Post snapshots that still differ from the current system.\nEntries already matching the restore target are hidden.\nRestoring reverts just those changes; files modified afterwards are left untouched.")
                  : qsTr("Shows the system state after applying the specified snapshot")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // 表示する差分と復元の向きの選択 (Pre/Postペアの場合だけ表示する)
        ThemedGroupBox {
            visible: root.isPrePostPair
            Layout.fillWidth: true

            ColumnLayout {
                spacing: 4

                RadioButton {
                    id: revertToPreRadioButton
                    checked: root.viewMode === root.viewRevertToPre
                    text: qsTr("Revert to Pre #%1: undo the changes made between Pre #%1 and Post #%2")
                              .arg(root.preSnapshotNumber)
                              .arg(root.postSnapshotNumber)
                    onClicked: root.switchPrePostDiffView(root.viewRevertToPre)
                }

                RadioButton {
                    id: reapplyToPostRadioButton
                    checked: root.viewMode === root.viewReapplyToPost
                    text: qsTr("Re-apply to Post #%2: redo the changes made between Pre #%1 and Post #%2")
                              .arg(root.preSnapshotNumber)
                              .arg(root.postSnapshotNumber)
                    onClicked: root.switchPrePostDiffView(root.viewReapplyToPost)
                }

                RadioButton {
                    id: preRadioButton
                    checked: root.viewMode === root.viewVsCurrent && root.activeSnapshotNumber === root.preSnapshotNumber
                    text: qsTr("Show differences between snapshot #%1 (Pre) and the current system (view only)").arg(root.preSnapshotNumber)
                    onClicked: root.switchSnapshotView(root.preSnapshotNumber)
                }

                RadioButton {
                    id: postRadioButton
                    checked: root.viewMode === root.viewVsCurrent && root.activeSnapshotNumber === root.postSnapshotNumber
                    text: qsTr("Show differences between snapshot #%1 (Post) and the current system (view only)").arg(root.postSnapshotNumber)
                    onClicked: root.switchSnapshotView(root.postSnapshotNumber)
                }
            }
        }

        // 変更一覧が上限を超えて省略されたことの通知 (省略された分は表示も復元もできないため、必ず見せる)
        Label {
            visible: root.changeModel.truncated
            text: qsTr("The list is too large and was truncated: %1 entries after the limit are omitted. Omitted entries are neither shown nor restored.").arg(root.changeModel.omittedCount)
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            color: root.warningTextColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // 左にファイルツリー、右に選択したファイルの詳細を表示する
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            // 左ペイン: 変更されたファイルのツリー
            Rectangle {
                SplitView.minimumWidth: 300
                SplitView.preferredWidth: 400
                color: palette.base
                border.color: palette.mid
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 5

                    // 差分がある場合だけツリーを表示する
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: fileChangeModel.hasChanges
                        clip: true
                        contentWidth: availableWidth

                        TreeView {
                            id: treeView
                            width: parent.width
                            model: fileChangeModel
                            delegate: treeDelegate
                            clip: true

                            selectionModel: ItemSelectionModel {
                                model: fileChangeModel
                            }

                            // ツリーの1行
                            Component {
                                id: treeDelegate

                                TreeViewDelegate {
                                    id: delegateItem

                                    // FileChangeModelのロール
                                    required property int changeType           // 変更種別 (0 = 作成、1 = 変更、2 = 削除、3 = 種別変更)
                                    required property bool isDirectory         // ディレクトリかどうか
                                    required property string fileName          // ファイル名またはディレクトリ名
                                    required property string filePath          // フルパス
                                    required property bool isChecked           // 復元対象としてチェックされているか
                                    required property string statusFlags       // Snapperのステータスフラグ (p / u / gなど)

                                    // 選択行とホバー行の背景 (ホバー色はLight/Darkで切り替える)
                                    background: Item {
                                        Rectangle {
                                            // インデントに関係なく、ペインの左端から右端まで塗る
                                            x: -delegateItem.x
                                            width: treeView.width
                                            height: parent.height
                                            color: delegateItem.current ? palette.highlight
                                                 : delegateItem.hovered ? (ThemeManager.isDark ? "#30FFFFFF" : "#20000000")
                                                 : "transparent"
                                        }
                                    }

                                    // チェックボックス、変更種別のバッジ、アイコン、名前を並べる
                                    contentItem: RowLayout {
                                        spacing: 5

                                        // 復元対象のチェックボックス (閲覧専用ビューでは復元できないため非表示)
                                        CheckBox {
                                            visible: root.restorable
                                            checked: isChecked
                                            onToggled: {
                                                fileChangeModel.setItemChecked(filePath, checked)
                                                // ディレクトリをチェックしたら、配下のファイルも見えるように全て展開する
                                                if (checked && isDirectory) {
                                                    treeView.expandRecursively(row, -1)
                                                }
                                            }
                                        }

                                        // 変更種別のバッジ (ディレクトリには付けない)
                                        Rectangle {
                                            width: 18
                                            height: 18
                                            radius: 2
                                            visible: !isDirectory
                                            color: {
                                                switch(changeType) {
                                                case 0: return ThemeManager.fileChangeCreated       // 作成 (緑)
                                                case 1: return ThemeManager.fileChangeModified      // 変更 (青)
                                                case 2: return ThemeManager.fileChangeDeleted       // 削除 (赤)
                                                case 3: return ThemeManager.fileChangeTypeChanged   // 種別変更 (オレンジ)
                                                default: return ThemeManager.snapshotTypeDefault
                                                }
                                            }

                                            Label {
                                                anchors.centerIn: parent
                                                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                                                text: {
                                                    switch(changeType) {
                                                    case 0: return "+"  // 作成
                                                    case 1: return "M"  // 変更
                                                    case 2: return "-"  // 削除
                                                    case 3: return "T"  // 種別変更
                                                    default: return "?"
                                                    }
                                                }
                                                color: "white"
                                                font.bold: true
                                                font.pixelSize: 9
                                            }
                                        }

                                        // ファイルかディレクトリかを示すアイコン
                                        Image {
                                            source: isDirectory ? "qrc:/QSnapper/icons/directory.svg" : "qrc:/QSnapper/icons/file.svg"
                                            width: 16
                                            height: 16
                                            visible: fileName !== ""
                                            sourceSize: Qt.size(16, 16)
                                        }

                                        // 名前 (名前の無いルートは(root)と表示する)
                                        Label {
                                            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                                            text: fileName || "(root)"
                                            color: palette.text
                                            font.italic: fileName === ""
                                            Layout.fillWidth: true
                                        }
                                    }

                                    // ファイルなら右ペインに詳細を表示し、ディレクトリなら右ペインを空にする
                                    onClicked: {
                                        if (!isDirectory) {
                                            rightPane.selectedFilePath = filePath
                                            rightPane.selectedChangeType = changeType
                                            rightPane.selectedStatusFlags = statusFlags
                                            rightPane.fileSelected = true
                                            rightPane.fileLoading = true

                                            // 差分と詳細をまとめて取得し、onFileDiffAndDetailsReadyで受け取る
                                            fileChangeModel.getFileDiffAndDetails(filePath)
                                        }
                                        else {
                                            rightPane.fileSelected = false
                                            rightPane.fileLoading = false
                                            diffTextArea.textFormat = TextEdit.PlainText
                                            diffTextArea.text = ""
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 差分が無い場合のメッセージ
                    // 読み込みに成功して0件だった場合だけ表示する (失敗時はエラー表示のみとし、差分が無いと誤解させない)
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.changeModel.loaded && !fileChangeModel.hasChanges

                        Label {
                            anchors.centerIn: parent
                            text: qsTr("No differences with snapshot")
                            color: palette.placeholderText
                            font.pixelSize: 14
                        }
                    }
                }
            }

            // 右ペイン: 選択したファイルの状態と差分
            Rectangle {
                id: rightPane
                SplitView.minimumWidth: 500
                SplitView.fillWidth: true
                color: palette.base
                border.color: palette.mid
                border.width: 1

                property bool fileSelected: false           // ファイルを選択中かどうか
                property bool fileLoading: false            // 差分と詳細を取得中かどうか
                property string selectedFilePath: ""        // 選択中のファイルのパス
                property int selectedChangeType: -1         // 選択中のファイルの変更種別 (-1 = 未選択)
                property string selectedStatusFlags: ""     // 選択中のファイルのステータスフラグ
                property var fileDetails: ({})              // 権限や所有者などの詳細

                /**
                 * @brief 差分を省略した理由を表示用の文言へ変換する
                 * @param reason サービスが返した省略理由 (binary / too_large / too_many_changes / special_file)
                 * @return 理由に対応する翻訳済みの説明文、未知の理由では汎用の文言
                 */
                function diffOmittedMessage(reason) {
                    switch (reason) {
                        case "binary":
                            return qsTr("The differences are not shown because the file is binary.")
                        case "too_large":
                            return qsTr("The differences are not shown because the file is too large.")
                        case "too_many_changes":
                            return qsTr("The differences are not shown because there are too many changes.")
                        case "special_file":
                            return qsTr("The differences are not shown because the file is not a regular file.")
                        default:
                            return qsTr("The differences are not shown.")
                    }
                }

                /**
                 * @brief HTMLの特殊文字を実体参照へエスケープする
                 * @param text エスケープ対象の文字列
                 * @return & < > を置換した文字列
                 */
                function escapeHtml(text) {
                    return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
                }

                /**
                 * @brief diffを行ごとに色分けしたHTMLへ変換する
                 * @param diffText diffの文字列
                 * @return pre要素で包んだHTML
                 * @note ヘッダ行、ハンク行、追加行、削除行をThemeManagerの色で塗り分ける
                 */
                function formatDiffHtml(diffText) {
                    var lines = diffText.split('\n')
                    var html = '<pre style="font-family: monospace; font-size: 14px; white-space: pre-wrap;">'
                    for (var i = 0; i < lines.length; i++) {
                        var line = escapeHtml(lines[i])
                        if (line.startsWith('+++') || line.startsWith('---')) {
                            html += '<span style="color: ' + ThemeManager.fileChangeModified + '; font-weight: bold;">' + line + '</span>\n'
                        } else if (line.startsWith('@@')) {
                            html += '<span style="color: ' + ThemeManager.fileChangeTypeChanged + ';">' + line + '</span>\n'
                        } else if (line.startsWith('+')) {
                            html += '<span style="color: ' + ThemeManager.fileChangeCreated + ';">' + line + '</span>\n'
                        } else if (line.startsWith('-')) {
                            html += '<span style="color: ' + ThemeManager.fileChangeDeleted + ';">' + line + '</span>\n'
                        } else {
                            html += line + '\n'
                        }
                    }
                    html += '</pre>'
                    return html
                }

                /**
                 * @brief 選択中ファイルの変更種別に対応する説明文を返す
                 * @return 変更種別に応じた翻訳済みの説明文
                 */
                function getContentStatusText() {
                    switch (selectedChangeType) {
                        case 0: return qsTr("New file was created.")
                        case 1: return qsTr("File content was modified.")
                        case 2: return qsTr("File was removed.")
                        case 3: return qsTr("File type was changed.")
                        default: return qsTr("File content was modified.")
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8

                    // ファイルを選ぶまでの案内
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: !rightPane.fileSelected

                        Label {
                            anchors.centerIn: parent
                            text: qsTr("Select a file to view details")
                            color: palette.placeholderText
                            font.pixelSize: 14
                        }
                    }

                    // 選択したファイルのパス
                    Label {
                        visible: rightPane.fileSelected
                        textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                        text: rightPane.selectedFilePath
                        font.bold: true
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }

                    // 詳細の取得中に表示する
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: rightPane.fileSelected && rightPane.fileLoading

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 10

                            BusyIndicator {
                                Layout.alignment: Qt.AlignHCenter
                                running: rightPane.fileLoading
                            }

                            Label {
                                text: qsTr("Loading file details...")
                                color: palette.placeholderText
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                    }

                    // ファイルの状態 (取得が終わってから表示する)
                    ThemedGroupBox {
                        visible: rightPane.fileSelected && !rightPane.fileLoading
                        title: qsTr("File Status")
                        Layout.fillWidth: true

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 4

                            // 内容の変更
                            Label {
                                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                                text: rightPane.getContentStatusText()
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                color: {
                                    switch (rightPane.selectedChangeType) {
                                        case 0: return ThemeManager.fileChangeCreated
                                        case 2: return ThemeManager.fileChangeDeleted
                                        default: return palette.text
                                    }
                                }
                            }

                            // 権限の変更 (ステータスフラグにpがある場合だけ表示する)
                            Label {
                                visible: rightPane.selectedStatusFlags.indexOf('p') !== -1
                                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                                text: {
                                    var d = rightPane.fileDetails
                                    var from = d["snapshotPerms"] || "?"
                                    var to = d["currentPerms"] || "?"
                                    return qsTr("File mode was changed from '%1' to '%2'.").arg(from).arg(to)
                                }
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            // 所有ユーザの変更 (uがある場合だけ表示する)
                            Label {
                                visible: rightPane.selectedStatusFlags.indexOf('u') !== -1
                                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                                text: {
                                    var d = rightPane.fileDetails
                                    var from = d["snapshotOwner"] || "?"
                                    var to = d["currentOwner"] || "?"
                                    return qsTr("File user ownership was changed from '%1' to '%2'.").arg(from).arg(to)
                                }
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            // 所有グループの変更 (gがある場合だけ表示する)
                            Label {
                                visible: rightPane.selectedStatusFlags.indexOf('g') !== -1
                                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                                text: {
                                    var d = rightPane.fileDetails
                                    var from = d["snapshotGroup"] || "?"
                                    var to = d["currentGroup"] || "?"
                                    return qsTr("File group ownership was changed from '%1' to '%2'.").arg(from).arg(to)
                                }
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }

                    // 色分けした差分
                    ScrollView {
                        visible: rightPane.fileSelected && !rightPane.fileLoading
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        TextArea {
                            id: diffTextArea
                            width: parent.width
                            readOnly: true
                            font.family: "Monospace"
                            font.pixelSize: 14
                            text: ""
                            wrapMode: TextArea.NoWrap
                            textFormat: TextEdit.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                            color: palette.text
                            background: Rectangle {
                                color: palette.base
                            }
                        }
                    }

                    // このファイルだけを復元するボタン (閲覧専用ビューでは非表示)
                    RowLayout {
                        visible: rightPane.fileSelected && !rightPane.fileLoading && root.restorable
                        Layout.fillWidth: true
                        spacing: 10

                        Item { Layout.fillWidth: true }

                        // 新規作成されたファイルは、復元すると削除されるため[Remove]と表示する
                        Button {
                            text: rightPane.selectedChangeType === 0
                                  ? qsTr("Remove")
                                  : qsTr("Restore")
                            highlighted: true
                            onClicked: {
                                confirmSingleRestoreDialog.open()
                            }
                        }
                    }
                }
            }
        }

        // エラーメッセージ (エラーが起きたときだけ表示する)
        Label {
            id: errorLabel
            textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
            Layout.fillWidth: true
            visible: false
            color: ThemeManager.errorColor
            wrapMode: Text.WordWrap
        }

        // ダイアログ下部のボタン
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 閲覧専用であることの案内
            // 無効なボタンにはツールチップが出ないため、ラベルで常に表示する
            Label {
                visible: root.isPrePostPair && !root.restorable
                text: qsTr("View only. Choose a Pre / Post difference above to restore.")
                color: palette.placeholderText
                wrapMode: Text.WordWrap
                Layout.maximumWidth: 420
            }

            Item {
                Layout.fillWidth: true
            }

            // キャンセルボタン
            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            // 復元ボタン (選択したスナップショットからファイルを復元する)
            Button {
                id: restoreButton
                text: (root.isPrePostPair && root.restorable)
                      ? qsTr("Restore Selected to #%1").arg(root.restoreSourceNumber)
                      : qsTr("Restore Selected")
                highlighted: true
                // 次のいずれかの場合は、ボタンを無効にする
                // 差分が1つもない場合、閲覧専用の表示の場合、復元するファイルに1つもチェックが入っていない場合
                // チェックが1つもないまま押しても、復元の開始時にエラーになるだけのため、押せないようにする
                enabled: fileChangeModel.hasChanges && root.restorable && root.hasCheckedItems
                opacity: enabled ? 1.0 : 0.45                            // 強調表示のボタンは無効でも見分けにくいため半透明にする
                onClicked: {
                    if (!root.restorable || root.restoreSourceNumber <= 0) return
                    confirmRestoreDialog.restoreTargetNumber = root.restoreSourceNumber

                    // 復元元に存在しないパスは、復元するとlive側から削除される
                    // 取り返しがつかないため、確認ダイアログを開く前に見せる
                    // FileChangeModel::checkedDeletePreviewを呼び出し、削除されるパスの総数と先頭からの一覧を取得する
                    var preview = root.changeModel.checkedDeletePreview(confirmRestoreDialog.maxDeletePathsShown)
                    confirmRestoreDialog.deleteCount = preview.count
                    confirmRestoreDialog.deletePaths = preview.paths
                    confirmRestoreDialog.deleteListed = preview.paths.length
                    confirmRestoreDialog.open()
                }
            }
        }
    }

    // 選択したファイルを復元する前の確認ダイアログ
    BorderedDialog {
        id: confirmRestoreDialog
        title: qsTr("Confirmation")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        // ダイアログの大きさは、ウィンドウの6割を目安にする (幅は700〜1100、高さは500〜950の範囲)
        // 説明文が長いため、小さいと何度も折り返して読みにくくなる
        // ただし、ウィンドウより大きいと画面からはみ出すため、ウィンドウより40だけ小さい値を上限にする
        width: {
            if (!ApplicationWindow.window) return 700
            var win = ApplicationWindow.window
            var calculated = Math.min(Math.max(win.width * 0.6, 700), 1100)
            return Math.min(calculated, win.width - 40)
        }
        // 削除対象のパスの一覧を表示する領域が必要なため、下限の高さを広げる
        height: {
            if (!ApplicationWindow.window) return 500
            var win = ApplicationWindow.window
            var calculated = Math.min(Math.max(win.height * 0.6, 600), 950)
            return Math.min(calculated, win.height - 40)
        }

        readonly property int maxDeletePathsShown: 2000  // 削除対象の一覧に載せるパスの上限 (一覧の生成と描画の負荷を抑える)
        property int restoreTargetNumber: 0              // 復元元のスナップショット番号 (開く前に設定する)
        property int deleteCount: 0                      // 復元でlive側から削除されるパスの総数 (開く前に設定する)
        property var deletePaths: []                     // 削除されるパスの一覧 (先頭からmaxDeletePathsShown件まで)
        property int deleteListed: 0                     // deletePathsに載せたパスの件数

        // ダイアログの本文 (上端から順に並べる)
        // 説明文を右端まで使って折り返すため、幅はダイアログいっぱいに広げる
        // 削除されるパスがある場合は、余った空白をパスの一覧に使うため、高さもダイアログに合わせる
        // ない場合は、項目の間が空いて間延びするため、必要な高さだけにする
        ColumnLayout {
            width: parent.width
            height: confirmRestoreDialog.deleteCount > 0 ? parent.height : implicitHeight
            spacing: 12

            Label {
                text: qsTr("Restore selected files/directories?")
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                text: root.isPrePostPair
                      ? qsTr("Only the selected entries of the Pre #%1 / Post #%2 difference will be restored to the snapshot #%3 state. Files changed outside this range are left untouched.")
                            .arg(root.preSnapshotNumber)
                            .arg(root.postSnapshotNumber)
                            .arg(confirmRestoreDialog.restoreTargetNumber)
                      : qsTr("This will restore selected files and directories to snapshot #%1 state.").arg(confirmRestoreDialog.restoreTargetNumber)
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                color: palette.text
            }

            // 上書きの警告 (警告文言の配色を他のラベルへ共有する取得元にもなる)
            Label {
                id: overwriteWarningLabel
                text: qsTr("Warning: This may overwrite current files.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                color: ThemeManager.warningColor
                font.italic: true
            }

            // 復元でlive側から削除されるパスの件数 (0件のときは表示しない)
            Label {
                visible: confirmRestoreDialog.deleteCount > 0
                text: qsTr("%1 path(s) will be DELETED from the current system:").arg(confirmRestoreDialog.deleteCount)
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                color: root.errorTextColor
                font.bold: true
            }

            // 削除されるパスの一覧 (パスが多くてもダイアログが伸びないよう、スクロールで見せる)
            // パスは外部由来の文字列のため、書式なしの読み取り専用テキストで表示する
            // 高さは余った空白まで広げ、ダイアログが小さくても数行は見えるよう110を下限にする
            // 基準の高さを指定しないとパスの全行分になるため、基準も110にする
            ScrollView {
                visible: confirmRestoreDialog.deleteCount > 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 110
                Layout.preferredHeight: 110
                clip: true

                TextArea {
                    readOnly: true
                    text: confirmRestoreDialog.deletePaths.join("\n")
                    textFormat: TextEdit.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                    wrapMode: TextEdit.NoWrap
                    font.family: "monospace"
                    font.pixelSize: 11
                }
            }

            // 一覧に載せきれなかった件数
            Label {
                visible: confirmRestoreDialog.deleteCount > confirmRestoreDialog.deleteListed
                text: qsTr("... and %1 more path(s) are not listed.").arg(confirmRestoreDialog.deleteCount - confirmRestoreDialog.deleteListed)
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // 復元オプション
            ThemedGroupBox {
                title: qsTr("Restore Options")
                Layout.fillWidth: true
                Layout.topMargin: 4

                // 復元方式とバッチサイズの設定欄 (左の列が項目名、右の列が入力欄)
                // 項目名を右揃えにして、ラジオボタンとスピンボックスの左端を揃える
                // 設定欄が左に寄らないよう、グループの枠の中央に置く
                GridLayout {
                    anchors.horizontalCenter: parent.horizontalCenter
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 8

                    // 復元方式 (直接コピーまたはYaST互換)
                    Label {
                        text: qsTr("Method:")
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    }
                    RowLayout {
                        spacing: 16
                        RadioButton {
                            id: directRestoreRadio
                            text: qsTr("Direct copy (fast)")
                            checked: fileChangeModel.useDirectRestore
                            onToggled: fileChangeModel.useDirectRestore = checked
                        }
                        RadioButton {
                            text: qsTr("YaST compatible")
                            checked: !fileChangeModel.useDirectRestore
                            onToggled: fileChangeModel.useDirectRestore = !checked
                        }
                    }

                    // 1回の要求でまとめて復元するファイル数
                    Label {
                        text: qsTr("Batch size:")
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    }
                    RowLayout {
                        spacing: 8
                        SpinBox {
                            id: batchSizeSpinBox
                            from: 1
                            to: 1000
                            value: fileChangeModel.restoreBatchSize
                            editable: true
                            onValueModified: fileChangeModel.restoreBatchSize = value
                        }
                        Label {
                            text: qsTr("files per batch")
                            color: palette.text
                        }
                    }
                }
            }
        }

        // 復元を始める
        // snapshotNumberを書き換えると比較モードが戻ってしまうため、復元元は引数で渡す
        // 前回の復元で表示したエラーは、今回の失敗と取り違えないよう先に消す
        onAccepted: {
            errorLabel.visible = false
            progressDialog.resetProgress()
            progressDialog.open()
            fileChangeModel.restoreCheckedItemsFrom(confirmRestoreDialog.restoreTargetNumber)
        }
    }

    // 復元の進捗ダイアログ (復元が終わるまで、ダイアログの外をクリックしても閉じない)
    BorderedDialog {
        id: progressDialog
        title: qsTr("Restoring Files")
        anchors.centerIn: Overlay.overlay
        modal: true
        closePolicy: Dialog.NoAutoClose
        width: {
            if (!ApplicationWindow.window) return 550
            var calculated = ApplicationWindow.window.width * 0.45
            return Math.min(Math.max(calculated, 550), 850)
        }
        height: {
            if (!ApplicationWindow.window) return 450
            var calculated = ApplicationWindow.window.height * 0.5
            return Math.min(Math.max(calculated, 450), 800)
        }
        standardButtons: Dialog.Cancel

        property int currentProgress: 0              // 処理済みのファイル数
        property int totalProgress: 0                // 全体のファイル数
        property string currentFile: ""              // 処理中のファイル

        // まだ画面に反映していない進捗 (flushTimerがまとめて反映する)
        property int pendingProgress: 0              // 最新の処理済みファイル数
        property int pendingTotal: 0                 // 最新の全体のファイル数
        property string pendingFile: ""              // 最新の処理中のファイル
        property var pendingLines: []                // 未反映のログ行
        property bool progressDirty: false           // 未反映の進捗があるかどうか (ログを載せない場合も進捗は反映するため、行数とは別に持つ)

        // ログの上限 (数十万件の復元でTextAreaが肥大化し、描画が止まるのを防ぐ)
        // 上限を超えた分は載せず、省略した件数を画面に表示する (進捗バーと件数は引き続き更新する)
        readonly property int maxLogLines: 2000      // ログに載せる行数の上限
        readonly property int maxLogChars: 1048576   // ログに載せる文字数の上限 (1 MiB)
        property int logLineCount: 0                 // ログに載せた行数
        property int logCharCount: 0                 // ログに載せた文字数
        property int logOmittedCount: 0              // 上限を超えてログに載せなかった件数

        // 進捗を100[ms]ごとに画面へ反映する
        // 反映するものが無くなったら止まり、次の進捗が届くと再開する
        Timer {
            id: flushTimer
            interval: 100
            repeat: true
            onTriggered: {
                // 反映していない進捗が無ければ止まる (ログを載せない場合も数値は更新するため、行数ではなく進捗の有無で判定する)
                if (!progressDialog.progressDirty) {
                    flushTimer.stop()
                    return
                }
                progressDialog.flushProgress()
            }
        }

        /**
         * @brief 届いた進捗を貯める
         * @param current 処理済みのファイル数
         * @param total 全体のファイル数
         * @param filePath 処理中のファイルのパス
         * @note 画面への反映はflushTimerがまとめて行う (止まっていれば開始する)
         *       ログの行数と文字数が上限に達した後は、行を貯めず省略した件数だけを数える
         */
        function queueProgress(current, total, filePath) {
            const pathLength = String(filePath).length  // 文字数の上限判定に使う (引数は型が不定のため文字列へ変換する)
            pendingProgress = current
            pendingTotal = total
            pendingFile = filePath
            progressDirty = true
            if (logLineCount < maxLogLines && logCharCount < maxLogChars) {
                pendingLines.push(filePath)
                logLineCount += 1
                logCharCount += pathLength + 1
            }
            else {
                logOmittedCount += 1
            }
            if (!flushTimer.running) {
                flushTimer.start()
            }
        }

        /**
         * @brief 貯めた進捗をまとめて画面に反映する
         * @note ログは追記し、最下行までスクロールする
         *       ログの上限を超えて載せる行が無い場合も、進捗の数値は更新する
         */
        function flushProgress() {
            if (!progressDirty) {
                return
            }
            // ProgressBarはvalueをtoの範囲に丸めるため、先にtotalProgressを更新する
            totalProgress = pendingTotal
            currentProgress = pendingProgress
            currentFile = pendingFile
            progressDirty = false

            // ログの上限を超えている場合は載せる行が無いため、数値の反映だけで終える
            if (pendingLines.length === 0) {
                return
            }
            // textを丸ごと置き換えると全体を作り直して重いため、追記だけにする
            restoreLogArea.append(pendingLines.join("\n"))
            pendingLines = []
            // text.lengthは全体を文字列にして重いため、lengthを使う
            restoreLogArea.cursorPosition = restoreLogArea.length
        }

        /**
         * @brief 進捗とログを初期状態に戻す
         * @note flushTimerも止める
         *       ログの行数・文字数・省略した件数も0に戻し、前回の復元の省略表示を引き継がない
         */
        function resetProgress() {
            flushTimer.stop()
            pendingProgress = 0
            pendingTotal = 0
            pendingFile = ""
            pendingLines = []
            progressDirty = false
            logLineCount = 0
            logCharCount = 0
            logOmittedCount = 0
            currentProgress = 0
            totalProgress = 0
            currentFile = ""
            restoreLogArea.clear()
        }

        contentItem: ColumnLayout {
            spacing: 10

            // 進捗メッセージ
            Label {
                text: qsTr("Restoring files. Please wait...")
                font.bold: true
                Layout.fillWidth: true
            }

            // プログレスバー
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: progressDialog.totalProgress
                value: progressDialog.currentProgress
            }

            // 進捗の件数
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                text: qsTr("Progress: %1 / %2").arg(progressDialog.currentProgress).arg(progressDialog.totalProgress)
                color: palette.text
            }

            // 復元したファイルのログ
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ScrollBar.vertical.policy: ScrollBar.AsNeeded
                ScrollBar.horizontal.policy: ScrollBar.AsNeeded

                // ログの追記と最下行へのスクロールはflushProgress()が行う
                TextArea {
                    id: restoreLogArea
                    readOnly: true
                    textFormat: TextEdit.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                    wrapMode: TextEdit.NoWrap
                    font.pixelSize: 11
                    font.family: "monospace"
                    color: palette.text
                    background: Rectangle {
                        color: palette.base
                        border.color: palette.mid
                        border.width: 1
                    }
                }
            }

            // ログの上限を超えて省略した件数 (省略が起きたときだけ表示する)
            Label {
                visible: progressDialog.logOmittedCount > 0
                Layout.fillWidth: true
                text: qsTr("The log shows only the first %1 entries. %2 further entries after that are omitted from the log (the restore itself continues).")
                          .arg(progressDialog.logLineCount).arg(progressDialog.logOmittedCount)
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                wrapMode: Text.WordWrap
                color: root.warningTextColor
            }

            // 処理中のファイル
            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: 500
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                text: progressDialog.currentFile
                wrapMode: Text.WrapAnywhere
                font.pixelSize: 10
                color: palette.placeholderText
            }
        }

        // [Cancel]ボタンで復元を中止する
        onRejected: {
            fileChangeModel.cancelRestore()
        }
    }

    // 1つのファイルを復元する前の確認ダイアログ
    BorderedDialog {
        id: confirmSingleRestoreDialog
        title: qsTr("Confirmation")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        width: {
            if (!ApplicationWindow.window) return 550
            var calculated = ApplicationWindow.window.width * 0.45
            return Math.min(Math.max(calculated, 550), 850)
        }
        height: {
            if (!ApplicationWindow.window) return 280
            var calculated = ApplicationWindow.window.height * 0.35
            return Math.min(Math.max(calculated, 280), 650)
        }

        ColumnLayout {
            spacing: 10

            Label {
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                text: rightPane.selectedChangeType === 0
                      ? qsTr("Remove this file from the current system?")
                      : qsTr("Restore this file from snapshot #%1?").arg(root.restoreSourceNumber)
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 450
            }

            Label {
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                text: rightPane.selectedFilePath
                wrapMode: Text.WrapAnywhere
                Layout.preferredWidth: 450
                color: palette.text
                font.family: "Monospace"
                font.pixelSize: 11
            }

            Label {
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                text: rightPane.selectedChangeType === 0
                      ? qsTr("Warning: This file will be deleted from the current system.")
                      : qsTr("Warning: The current file will be overwritten.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 450
                color: ThemeManager.warningColor
                font.italic: true
            }
        }

        // 進捗ダイアログを1件分の表示にして復元を始める
        onAccepted: {
            if (!root.restorable || root.restoreSourceNumber <= 0) return

            // 前回の復元で表示したエラーは、今回の失敗と取り違えないよう先に消す
            errorLabel.visible = false
            progressDialog.resetProgress()
            progressDialog.totalProgress = 1
            progressDialog.currentFile = rightPane.selectedFilePath
            progressDialog.open()
            fileChangeModel.restoreSingleFileFrom(rightPane.selectedFilePath, root.restoreSourceNumber)
        }
    }

    // 復元の成功を伝えるダイアログ
    BorderedDialog {
        id: successDialog
        title: qsTr("Success")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok
        width: {
            if (!ApplicationWindow.window) return 500
            var calculated = ApplicationWindow.window.width * 0.4
            return Math.min(Math.max(calculated, 500), 800)
        }
        // 復元結果の件数と失敗位置を表示する領域が必要なため、他のダイアログより高さを確保する
        height: {
            if (!ApplicationWindow.window) return 220
            var calculated = ApplicationWindow.window.height * 0.4
            return Math.min(Math.max(calculated, 340), 650)
        }

        ColumnLayout {
            spacing: 10

            Label {
                text: qsTr("File/directory restoration completed.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
            }

            // Pre/Postペアでは、復元後に表示を切り替えたことを伝える
            Label {
                visible: root.isPrePostPair
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                text: qsTr("Switched to the differences between snapshot #%1 and the current system.")
                        .arg(root.activeSnapshotNumber)
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
            }

            // 適用・削除・スキップの件数 (サーバの状態応答が届いてから表示する)
            RestoreResultSummary {
                result: root.restoreResult
                errorTextColor: root.errorTextColor
                Layout.fillWidth: true
                Layout.preferredWidth: 400
            }

            // 再読み込みが終わり、差分が残っていない場合だけ表示する
            // 読み込みに失敗した場合は差分が無いとは限らないため、読み込みに成功したときだけ表示する
            Label {
                visible: root.changeModel.loaded && !fileChangeModel.hasChanges && !fileChangeModel.loading
                text: qsTr("No more differences with snapshot.")
                color: ThemeManager.successColor
                font.italic: true
                Layout.preferredWidth: 400
            }
        }

        // 復元が済んだので、差分ダイアログも閉じる
        onAccepted: {
            root.close()
        }
    }

    // 復元の失敗を伝えるダイアログ
    BorderedDialog {
        id: restoreFailDialog
        title: qsTr("Restore Failed")
        property string errorMessage: ""  // 復元を始められなかった理由や認可の拒否など (開く前に設定する)
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok
        width: {
            if (!ApplicationWindow.window) return 500
            var calculated = ApplicationWindow.window.width * 0.4
            return Math.min(Math.max(calculated, 500), 800)
        }
        // 復元結果の件数と失敗位置を表示する領域が必要なため、他のダイアログより高さを確保する
        height: {
            if (!ApplicationWindow.window) return 220
            var calculated = ApplicationWindow.window.height * 0.4
            return Math.min(Math.max(calculated, 340), 650)
        }

        ColumnLayout {
            spacing: 10

            Label {
                text: qsTr("Failed to restore some or all files.")
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
            }

            // エラーの原因 (認可の拒否など、読める文言が得られた場合だけ表示する)
            Label {
                visible: restoreFailDialog.errorMessage !== ""
                text: restoreFailDialog.errorMessage
                textFormat: Text.PlainText  // 表示する文字列にタグが含まれていても、リッチテキストとして解釈させない
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: root.errorTextColor
            }

            Label {
                text: qsTr("The files may already be in sync with the snapshot, or an error occurred during restoration. Check the system log for details.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: ThemeManager.warningColor
            }

            // 適用・削除・スキップの件数と、失敗したエントリの位置 (サーバの状態応答が届いてから表示する)
            RestoreResultSummary {
                result: root.restoreResult
                errorTextColor: root.errorTextColor
                Layout.fillWidth: true
                Layout.preferredWidth: 400
            }
        }
    }
}
