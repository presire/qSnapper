/**
 * @file RestorePreviewDialog.qml
 * @brief スナップショットとの差分を確認し、選択したファイルを復元するダイアログ
 *
 * SnapshotDetailDialogから開く
 * 復元はFileChangeModelがまとめて行うため、Polkitの認証は1回で済む
 * Pre/Postペアでは、Pre <--> Post間の差分ビューからだけ復元できる (「vs 現在」ビューは閲覧専用)
 *
 * 入力 : configName、snapshotNumber、preSnapshotNumber、postSnapshotNumber (親からバインド)
 * 出力 : restoreConfirmed()
 * 依存 : ThemeManager (配色)、FileChangeModel (差分ツリーと復元)、BorderedDialog (枠付きダイアログ)、
 *        ThemedGroupBox (枠付きグループ)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property string configName: "root"               // Snapper設定名
    property int snapshotNumber: 0                   // 開いたときに表示するスナップショット番号
    property int preSnapshotNumber: 0                // Preスナップショット番号 (0 = Pre/Postペアではない)
    property int postSnapshotNumber: 0               // Postスナップショット番号
    // snapshotNumberは親からバインドされており、書き換えるとバインドが外れるため、表示の切り替えにはこちらを使う
    property int activeSnapshotNumber: 0             // 現在表示中のスナップショット番号
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
            text: qsTr("Snapper configuration: %1").arg(root.configName)
            font.bold: true
        }

        // 現在の表示内容の説明
        Label {
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
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: !fileChangeModel.hasChanges

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
                            textFormat: TextEdit.PlainText
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
        height: {
            if (!ApplicationWindow.window) return 500
            var win = ApplicationWindow.window
            var calculated = Math.min(Math.max(win.height * 0.6, 500), 950)
            return Math.min(calculated, win.height - 40)
        }

        property int restoreTargetNumber: 0  // 復元元のスナップショット番号 (開く前に設定する)

        // ダイアログの本文 (確認の質問、復元の内容、注意書き、復元オプション)
        // 幅はダイアログいっぱいに広げ、説明文がダイアログの右端まで使って折り返すようにする
        // 本文が上に寄って下に大きな空白が残らないよう、縦方向は中央に置く
        // ただし、本文がダイアログに収まらない場合は、先頭が隠れないよう上端から表示する
        ColumnLayout {
            width: parent.width
            y: Math.max(0, (parent.height - height) / 2)
            spacing: 12

            Label {
                text: qsTr("Restore selected files/directories?")
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
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

            Label {
                text: qsTr("Warning: This may overwrite current files.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                color: ThemeManager.warningColor
                font.italic: true
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
        onAccepted: {
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

        // 進捗を100[ms]ごとに画面へ反映する
        // 反映するものが無くなったら止まり、次の進捗が届くと再開する
        Timer {
            id: flushTimer
            interval: 100
            repeat: true
            onTriggered: {
                if (progressDialog.pendingLines.length === 0) {
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
         */
        function queueProgress(current, total, filePath) {
            pendingProgress = current
            pendingTotal = total
            pendingFile = filePath
            pendingLines.push(filePath)
            if (!flushTimer.running) {
                flushTimer.start()
            }
        }

        /**
         * @brief 貯めた進捗をまとめて画面に反映する
         * @note ログは追記し、最下行までスクロールする
         */
        function flushProgress() {
            if (pendingLines.length === 0) {
                return
            }
            // ProgressBarはvalueをtoの範囲に丸めるため、先にtotalProgressを更新する
            totalProgress = pendingTotal
            currentProgress = pendingProgress
            currentFile = pendingFile
            // textを丸ごと置き換えると全体を作り直して重いため、追記だけにする
            restoreLogArea.append(pendingLines.join("\n"))
            pendingLines = []
            // text.lengthは全体を文字列にして重いため、lengthを使う
            restoreLogArea.cursorPosition = restoreLogArea.length
        }

        /**
         * @brief 進捗とログを初期状態に戻す
         * @note flushTimerも止める
         */
        function resetProgress() {
            flushTimer.stop()
            pendingProgress = 0
            pendingTotal = 0
            pendingFile = ""
            pendingLines = []
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

            // 処理中のファイル
            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: 500
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
                text: rightPane.selectedChangeType === 0
                      ? qsTr("Remove this file from the current system?")
                      : qsTr("Restore this file from snapshot #%1?").arg(root.restoreSourceNumber)
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 450
            }

            Label {
                text: rightPane.selectedFilePath
                wrapMode: Text.WrapAnywhere
                Layout.preferredWidth: 450
                color: palette.text
                font.family: "Monospace"
                font.pixelSize: 11
            }

            Label {
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
        height: {
            if (!ApplicationWindow.window) return 220
            var calculated = ApplicationWindow.window.height * 0.3
            return Math.min(Math.max(calculated, 220), 600)
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
                text: qsTr("Switched to the differences between snapshot #%1 and the current system.")
                        .arg(root.activeSnapshotNumber)
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
            }

            // 再読み込みが終わり、差分が残っていない場合だけ表示する
            Label {
                visible: !fileChangeModel.hasChanges && !fileChangeModel.loading
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
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok
        width: {
            if (!ApplicationWindow.window) return 500
            var calculated = ApplicationWindow.window.width * 0.4
            return Math.min(Math.max(calculated, 500), 800)
        }
        height: {
            if (!ApplicationWindow.window) return 220
            var calculated = ApplicationWindow.window.height * 0.3
            return Math.min(Math.max(calculated, 220), 600)
        }

        ColumnLayout {
            spacing: 10

            Label {
                text: qsTr("Failed to restore some or all files.")
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
            }

            Label {
                text: qsTr("The files may already be in sync with the snapshot, or an error occurred during restoration. Check the system log for details.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: ThemeManager.warningColor
            }
        }
    }
}
