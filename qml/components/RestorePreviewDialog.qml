// ファイル復元プレビューダイアログ
// スナップショットとの差分を階層的に表示し、
// 選択したファイル/ディレクトリを復元する機能を提供
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property string configName: "root"               // Snapper設定名
    property int snapshotNumber: 0                   // 親から渡される初期スナップショット番号 (入力専用)
    property int preSnapshotNumber: 0                // Preスナップショット番号 (0 = Pre/Postペアではない)
    property int postSnapshotNumber: 0               // Postスナップショット番号
    // 現在のTreeViewに表示中のスナップショット番号 (ラジオボタン切替で変更される)
    // snapshotNumberは親のQMLバインディング経由で渡されるため、ここで上書きするとバインディングが壊れ次回以降の表示が不正になる
    // そのため内部状態は分離する。
    property int activeSnapshotNumber: 0
    readonly property bool isPrePostPair: preSnapshotNumber > 0 && postSnapshotNumber > 0

    // 表示モード定数
    // FileChangeModelは比較の第1引数を復元元として扱うため、Pre <--> Post差分の向きは復元先で決まる
    // 向きはラジオ選択の時点で確定させる (復元ボタン押下時に読み直すとユーザのチェック選択が消えるため)
    readonly property int viewRevertToPre: 0     // Pre <--> Post差分をPreへ戻す向き (復元可)
    readonly property int viewReapplyToPost: 1   // Pre <--> Post差分をPostへ再適用する向き (復元可)
    readonly property int viewVsCurrent: 2       // 選択スナップショットと現在のシステムの差分 (Pre/Postペアでは閲覧専用)
    property int viewMode: root.viewVsCurrent

    // Pre <--> Post間の差分を表示しているかどうか
    readonly property bool prePostDiffMode: root.viewMode === root.viewRevertToPre
                                            || root.viewMode === root.viewReapplyToPost

    // 現在の表示から復元できるかどうか
    // Pre/Postペアでは、Pre <--> Post間で起きた変更だけを巻き戻せる差分ビューのみ復元を許可し、
    // 「vs 現在」ビューは閲覧専用とする (そちらから復元すると、Post以降の手動変更まで巻き戻るため)
    readonly property bool restorable: root.isPrePostPair ? root.prePostDiffMode : true

    // 復元元スナップショット番号 (0 = 現在の表示からは復元できない)
    readonly property int restoreSourceNumber: {
        if (!root.isPrePostPair) return root.activeSnapshotNumber
        if (root.viewMode === root.viewRevertToPre) return root.preSnapshotNumber
        if (root.viewMode === root.viewReapplyToPost) return root.postSnapshotNumber
        return 0
    }

    signal restoreConfirmed()                        // 復元確認シグナル

    // 右ペインの選択状態・diff表示をクリアする共通ヘルパ
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

    // Pre/Post または Single --> 対カレント比較
    function switchSnapshotView(targetNumber) {
        if (root.viewMode === root.viewVsCurrent && root.activeSnapshotNumber === targetNumber) return
        root.viewMode = root.viewVsCurrent
        root.activeSnapshotNumber = targetNumber
        fileChangeModel.snapshotNumber = targetNumber  // betweenMode/flatModeをfalseにリセット
        resetRightPane()
        fileChangeModel.loadChanges()                  // ファイルツリーを再読み込み (切替先スナップショット vs 現在のシステムで比較)
    }

    // Pre <--> Post間の差分ビューに切り替える
    function switchPrePostDiffView(mode) {
        if (!root.isPrePostPair) return
        if (root.viewMode === mode) return
        root.viewMode = mode
        resetRightPane()
        loadPrePostDiff()
    }

    // 現在のviewModeに対応する向きでPre <--> Post差分を読み込む
    // 第1引数が復元元になるため、復元先スナップショットを先頭に置いて向きを揃える
    // 第4引数のtrueで、復元元と現在のシステムの間に差異が無いエントリ (既に復元済みなど) を非表示にする
    function loadPrePostDiff() {
        if (root.viewMode === root.viewRevertToPre) {
            fileChangeModel.loadChangesBetween(root.preSnapshotNumber,   // flat=falseでツリー構築 (TreeView表示)
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

    // 現在の表示条件を保ったままファイルツリーを再読み込みする
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

    // ダイアログ表示時の初期化処理
    onOpened: {
        console.log("RestorePreviewDialog opened with configName:", configName, "snapshotNumber:", snapshotNumber)
        errorLabel.visible = false
        fileChangeModel.configName = configName

        // 前回表示したスナップショットの右ペイン状態 (選択ファイル・diff等) を引き継がないようにクリアする
        resetRightPane()

        // 毎回開いた時点のsnapshotNumberを現在表示用の状態に同期する
        root.activeSnapshotNumber = snapshotNumber

        if (root.isPrePostPair) {
            // Pre/Post ペア: 既定でPre <--> Post間の差分をPreへ戻す向きで表示する
            root.viewMode = root.viewRevertToPre
            loadPrePostDiff()
        }
        else {
            root.viewMode = root.viewVsCurrent
            fileChangeModel.snapshotNumber = snapshotNumber
            fileChangeModel.loadChanges()
        }
    }

    // ダイアログを閉じた際の状態クリア
    // 次回別スナップショットで開かれた場合に前回のdiffが残らないようにする
    onClosed: {
        resetRightPane()
    }

    // ファイル変更モデル
    // スナップショットとの差分を階層的に管理
    FileChangeModel {
        id: fileChangeModel

        // エラー発生時のハンドラ
        onErrorOccurred: function(message) {
            errorLabel.text = message
            errorLabel.visible = true
        }

        // ファイル差分＋詳細情報の非同期結果ハンドラ
        onFileDiffAndDetailsReady: function(filePath, details, diff) {
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

        // 復元進捗更新ハンドラ
        // シグナルは復元ファイル1件ごとに届くため、ここではバッファへ積むだけにする
        // (1件ごとにUIを書き換えるとGUIスレッドが占有され、描画が更新されなくなる)
        onRestoreProgress: function(current, total, filePath) {
            progressDialog.queueProgress(current, total, filePath)
        }

        // 復元完了ハンドラ
        onRestoreCompleted: function(success) {
            progressDialog.close()
            if (success) {
                // 右ペインの選択状態をリセット
                root.resetRightPane()

                // 復元後は現在のシステムの状態を一覧へ反映させる。
                // Pre<-->Post差分はスナップショット同士の比較であり、復元しても内容は変わらないため、
                // Pre/Postペアでは復元先スナップショット vs 現在 へ切り替える。
                // このビューには範囲外で追加されたファイルも現れるため、意図しない巻き戻りが無いことも確認できる。
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
                // 復元失敗時のエラーフィードバック
                restoreFailDialog.open()
            }
        }
    }

    // 読み込み中のオーバーレイ
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

    // メインレイアウト
    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        visible: !fileChangeModel.loading

        // 説明ヘッダ
        Label {
            text: qsTr("Root Filesystem")
            font.bold: true
        }

        Label {
            text: root.prePostDiffMode
                  ? qsTr("Shows only the changes made between the Pre and Post snapshots that still differ from the current system. Entries already matching the restore target are hidden. Restoring reverts just those changes; files modified afterwards are left untouched.")
                  : qsTr("Shows the system state after applying the specified snapshot")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // Pre/Postペア時: 差分対象と復元の向きを切り替えるラジオボタン
        GroupBox {
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

        // メインコンテンツ: 左右分割ビュー
        // 左: ファイルツリー、右: 差分表示
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            // 左ペイン: ファイル変更ツリービュー
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

                    // 変更がある場合: ファイル変更ツリー表示
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: fileChangeModel.hasChanges
                        clip: true
                        contentWidth: availableWidth

                        // ファイル変更階層ツリービュー
                        TreeView {
                            id: treeView
                            width: parent.width
                            model: fileChangeModel
                            delegate: treeDelegate
                            clip: true

                            selectionModel: ItemSelectionModel {
                                model: fileChangeModel
                            }

                            // ツリーアイテムデリゲート
                            Component {
                                id: treeDelegate

                                TreeViewDelegate {
                                    id: delegateItem

                                    // モデルから自動的に提供されるプロパティ
                                    required property int changeType           // 変更タイプ (作成/変更/削除/タイプ変更)
                                    required property bool isDirectory         // ディレクトリフラグ
                                    required property string fileName          // ファイル/ディレクトリ名
                                    required property string filePath          // フルパス
                                    required property bool isChecked           // 復元選択状態
                                    required property string statusFlags       // 詳細ステータスフラグ

                                    // 選択・ホバー時の背景をペイン全幅に表示
                                    background: Item {
                                        Rectangle {
                                            // デリゲートのインデントを打ち消してTreeView左端から右端まで描画
                                            x: -delegateItem.x
                                            width: treeView.width
                                            height: parent.height
                                            color: delegateItem.current ? palette.highlight
                                                 : delegateItem.hovered ? (ThemeManager.isDark ? "#30FFFFFF" : "#20000000")
                                                 : "transparent"
                                        }
                                    }

                                    contentItem: RowLayout {
                                        spacing: 5

                                        // 復元選択チェックボックス
                                        // 閲覧専用ビュー (Pre/Postペアの「vs 現在」) では復元できないため非表示
                                        CheckBox {
                                            visible: root.restorable
                                            checked: isChecked
                                            onToggled: {
                                                fileChangeModel.setItemChecked(filePath, checked)
                                                // ディレクトリで、チェックがONの場合は再帰的に展開
                                                if (checked && isDirectory) {
                                                    treeView.expandRecursively(row, -1)
                                                }
                                            }
                                        }

                                        // 変更タイプバッジ (ファイルのみ)
                                        Rectangle {
                                            width: 18
                                            height: 18
                                            radius: 2
                                            visible: !isDirectory
                                            color: {
                                                switch(changeType) {
                                                case 0: return ThemeManager.fileChangeCreated       // Created - 緑
                                                case 1: return ThemeManager.fileChangeModified      // Modified - 青
                                                case 2: return ThemeManager.fileChangeDeleted       // Deleted - 赤
                                                case 3: return ThemeManager.fileChangeTypeChanged   // TypeChanged - オレンジ
                                                default: return ThemeManager.snapshotTypeDefault
                                                }
                                            }

                                            Label {
                                                anchors.centerIn: parent
                                                text: {
                                                    switch(changeType) {
                                                    case 0: return "+"  // Created
                                                    case 1: return "M"  // Modified
                                                    case 2: return "-"  // Deleted
                                                    case 3: return "T"  // TypeChanged
                                                    default: return "?"
                                                    }
                                                }
                                                color: "white"
                                                font.bold: true
                                                font.pixelSize: 9
                                            }
                                        }

                                        // ファイル/ディレクトリ種別アイコン
                                        Image {
                                            source: isDirectory ? "qrc:/QSnapper/icons/directory.svg" : "qrc:/QSnapper/icons/file.svg"
                                            width: 16
                                            height: 16
                                            visible: fileName !== ""
                                            sourceSize: Qt.size(16, 16)
                                        }

                                        // ファイル名/ディレクトリ名表示
                                        Label {
                                            text: fileName || "(root)"
                                            color: palette.text
                                            font.italic: fileName === ""
                                            Layout.fillWidth: true
                                        }
                                    }

                                    // クリック時: 右ペインにファイル詳細を非同期で表示
                                    onClicked: {
                                        if (!isDirectory) {
                                            // 選択ファイル情報を右ペインに設定
                                            rightPane.selectedFilePath = filePath
                                            rightPane.selectedChangeType = changeType
                                            rightPane.selectedStatusFlags = statusFlags
                                            rightPane.fileSelected = true
                                            rightPane.fileLoading = true

                                            // 非同期で統合リクエスト
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

                    // 変更がない場合: メッセージ表示
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

            // 右ペイン: ファイル詳細・差分表示
            Rectangle {
                id: rightPane
                SplitView.minimumWidth: 500
                SplitView.fillWidth: true
                color: palette.base
                border.color: palette.mid
                border.width: 1

                // 選択ファイルの状態プロパティ
                property bool fileSelected: false
                property bool fileLoading: false
                property string selectedFilePath: ""
                property int selectedChangeType: -1
                property string selectedStatusFlags: ""
                property var fileDetails: ({})

                // 差分を省略した理由 (details部のdiffOmitted) を表示用の文言に変換
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

                // HTMLエスケープ
                function escapeHtml(text) {
                    return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
                }

                // diffテキストをカラーHTML に変換
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

                // コンテンツ変更テキストを取得
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

                    // ファイル未選択時のプレースホルダー
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

                    // ファイル選択時のコンテンツ
                    // ファイルパスヘッダ
                    Label {
                        visible: rightPane.fileSelected
                        text: rightPane.selectedFilePath
                        font.bold: true
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }

                    // ローディング中のインジケーター
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

                    // ステータス情報セクション
                    GroupBox {
                        visible: rightPane.fileSelected && !rightPane.fileLoading
                        title: qsTr("File Status")
                        Layout.fillWidth: true

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 4

                            // コンテンツ変更ステータス
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

                            // パーミッション変更
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

                            // ユーザ所有者変更
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

                            // グループ所有者変更
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

                    // カラーdiff表示
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

                    // 個別ファイル復元ボタン
                    // 閲覧専用ビューでは復元できないため非表示
                    RowLayout {
                        visible: rightPane.fileSelected && !rightPane.fileLoading && root.restorable
                        Layout.fillWidth: true
                        spacing: 10

                        Item { Layout.fillWidth: true }

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

        // エラーメッセージ表示ラベル
        Label {
            id: errorLabel
            Layout.fillWidth: true
            visible: false
            color: ThemeManager.errorColor
            wrapMode: Text.WordWrap
        }

        // アクションボタン行
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 閲覧専用ビューであることの案内
            // 無効化されたButtonはホバーを受け取らずToolTipが出ないため、常設のラベルで示す
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

            // 復元ボタン
            // 復元元はラジオ選択で確定済みのrestoreSourceNumberを使う
            Button {
                id: restoreButton
                text: (root.isPrePostPair && root.restorable)
                      ? qsTr("Restore Selected to #%1").arg(root.restoreSourceNumber)
                      : qsTr("Restore Selected")
                highlighted: true
                enabled: fileChangeModel.hasChanges && root.restorable
                onClicked: {
                    if (!root.restorable || root.restoreSourceNumber <= 0) return
                    confirmRestoreDialog.restoreTargetNumber = root.restoreSourceNumber
                    confirmRestoreDialog.open()
                }
            }
        }
    }

    // ファイル復元確認ダイアログ
    BorderedDialog {
        id: confirmRestoreDialog
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
            if (!ApplicationWindow.window) return 380
            var calculated = ApplicationWindow.window.height * 0.45
            return Math.min(Math.max(calculated, 380), 750)
        }

        property int restoreTargetNumber: 0  // 復元対象のスナップショット番号 (open前に明示的にセット)

        ColumnLayout {
            spacing: 10

            Label {
                text: qsTr("Restore selected files/directories?")
                font.bold: true
            }

            Label {
                text: root.isPrePostPair
                      ? qsTr("Only the selected entries of the Pre #%1 / Post #%2 difference will be restored to the snapshot #%3 state. Files changed outside this range are left untouched.")
                            .arg(root.preSnapshotNumber)
                            .arg(root.postSnapshotNumber)
                            .arg(confirmRestoreDialog.restoreTargetNumber)
                      : qsTr("This will restore selected files and directories to snapshot #%1 state.").arg(confirmRestoreDialog.restoreTargetNumber)
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 450
                color: palette.text
            }

            Label {
                text: qsTr("Warning: This may overwrite current files.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 450
                color: ThemeManager.warningColor
                font.italic: true
            }

            // 復元オプション
            GroupBox {
                title: qsTr("Restore Options")
                Layout.fillWidth: true

                ColumnLayout {
                    spacing: 8

                    // 復元方式選択
                    RowLayout {
                        spacing: 10
                        Label {
                            text: qsTr("Method:")
                        }
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

                    // バッチサイズ設定
                    RowLayout {
                        spacing: 10
                        Label {
                            text: qsTr("Batch size:")
                        }
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

        // 復元実行
        // snapshotNumberへの代入は比較モードをリセットしてしまうため、復元元は引数で渡す
        onAccepted: {
            progressDialog.resetProgress()
            progressDialog.open()
            fileChangeModel.restoreCheckedItemsFrom(confirmRestoreDialog.restoreTargetNumber)
        }
    }

    // 復元進捗表示ダイアログ
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

        property int currentProgress: 0              // 現在の進捗
        property int totalProgress: 0                // 総ファイル数
        property string currentFile: ""              // 現在処理中のファイル

        // 進捗シグナルの保留分 (flushTimerで一括反映する)
        property int pendingProgress: 0              // 最新の進捗
        property int pendingTotal: 0                 // 最新の総ファイル数
        property string pendingFile: ""              // 最新の処理中ファイル
        property var pendingLines: []                // 未反映のログ行

        // 保留分を一定間隔でUIへ反映する
        // 保留が無くなったら停止し、次のqueueProgressで再開する
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

        // 受信した進捗をバッファへ蓄積する (UIへは反映しない)
        function queueProgress(current, total, filePath) {
            pendingProgress = current
            pendingTotal = total
            pendingFile = filePath
            pendingLines.push(filePath)
            if (!flushTimer.running) {
                flushTimer.start()
            }
        }

        // 保留中の進捗をUIへ一括反映する
        function flushProgress() {
            if (pendingLines.length === 0) {
                return
            }
            // ProgressBarはvalueをtoの範囲へ丸めるため、totalProgressを先に更新する
            totalProgress = pendingTotal
            currentProgress = pendingProgress
            currentFile = pendingFile
            // textの全置換はドキュメント全体を再構築するため、増分のみ追記する
            restoreLogArea.append(pendingLines.join("\n"))
            pendingLines = []
            // lengthはO(1)。text.lengthはドキュメント全体を文字列化するため使わない
            restoreLogArea.cursorPosition = restoreLogArea.length
        }

        // 進捗表示とログを初期化する
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

            // 進捗数値表示
            Label {
                Layout.fillWidth: true
                text: qsTr("Progress: %1 / %2").arg(progressDialog.currentProgress).arg(progressDialog.totalProgress)
                color: palette.text
            }

            // 復元済みファイルのログ表示
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ScrollBar.vertical.policy: ScrollBar.AsNeeded
                ScrollBar.horizontal.policy: ScrollBar.AsNeeded

                // テキストはflushProgress()がappend()で増分追記し、
                // 最下行への自動スクロールもそこでcursorPositionを更新して行う
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

            // 現在処理中のファイル名表示
            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: 500
                text: progressDialog.currentFile
                wrapMode: Text.WrapAnywhere
                font.pixelSize: 10
                color: palette.placeholderText
            }
        }

        // キャンセル時
        onRejected: {
            fileChangeModel.cancelRestore()
        }
    }

    // 個別ファイル復元確認ダイアログ
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

        onAccepted: {
            if (!root.restorable || root.restoreSourceNumber <= 0) return
            progressDialog.resetProgress()
            progressDialog.totalProgress = 1
            progressDialog.currentFile = rightPane.selectedFilePath
            progressDialog.open()
            fileChangeModel.restoreSingleFileFrom(rightPane.selectedFilePath, root.restoreSourceNumber)
        }
    }

    // 復元成功ダイアログ
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

            Label {
                visible: root.isPrePostPair
                text: qsTr("Switched to the differences between snapshot #%1 and the current system.")
                        .arg(root.activeSnapshotNumber)
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
            }

            Label {
                visible: !fileChangeModel.hasChanges && !fileChangeModel.loading
                text: qsTr("No more differences with snapshot.")
                color: ThemeManager.successColor
                font.italic: true
                Layout.preferredWidth: 400
            }
        }

        onAccepted: {
            root.close()
        }
    }

    // 復元失敗ダイアログ
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
