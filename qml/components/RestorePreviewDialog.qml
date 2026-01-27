// ファイル復元プレビューダイアログ
// スナップショットとの差分を階層的に表示し、
// 選択したファイル/ディレクトリを復元する機能を提供
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

Dialog {
    id: root

    property string configName: "root"               // Snapper設定名
    property int snapshotNumber: 0                   // 対象スナップショット番号

    signal restoreConfirmed()                        // 復元確認シグナル

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
        fileChangeModel.snapshotNumber = snapshotNumber
        fileChangeModel.loadChanges()
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

        // 復元進捗更新ハンドラ
        onRestoreProgress: function(current, total, filePath) {
            progressDialog.currentFile = filePath
            progressDialog.currentProgress = current
            progressDialog.totalProgress = total
        }

        // 復元完了ハンドラ
        onRestoreCompleted: function(success) {
            progressDialog.close()
            if (success) {
                // 復元成功後、データを再読み込みして最新状態を反映
                fileChangeModel.loadChanges()
                successDialog.open()
            }
        }
    }

    // メインレイアウト
    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // 説明ヘッダー
        Label {
            text: qsTr("Root Filesystem")
            font.bold: true
        }

        Label {
            text: qsTr("Shows the system state after applying the specified snapshot")
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

                        // ファイル変更階層ツリービュー
                        TreeView {
                            id: treeView
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

                                    contentItem: RowLayout {
                                        spacing: 5

                                        // 復元選択チェックボックス
                                        CheckBox {
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

                                    // クリック時: 右ペインにdiffを表示
                                    onClicked: {
                                        if (!isDirectory) {
                                            diffTextArea.text = qsTr("Loading diff...")
                                            var diff = fileChangeModel.getFileDiff(filePath)

                                            if (diff === "") {
                                                // 新規作成されたファイル
                                                if (changeType === 0) {
                                                    diffTextArea.text = qsTr("New file created.")
                                                } else if (changeType === 2) {
                                                    diffTextArea.text = qsTr("File deleted.")
                                                } else {
                                                    diffTextArea.text = qsTr("No diff found.")
                                                }
                                            } else {
                                                diffTextArea.text = diff
                                            }
                                        } else {
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

            // 右ペイン: ファイル差分(diff)表示
            Rectangle {
                SplitView.minimumWidth: 500
                SplitView.fillWidth: true
                color: palette.base
                border.color: palette.mid
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 5

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        // diff表示用テキストエリア
                        TextArea {
                            id: diffTextArea
                            readOnly: true
                            font.family: "Monospace"
                            font.pixelSize: 11
                            text: ""
                            wrapMode: TextArea.NoWrap
                            textFormat: TextEdit.PlainText
                            color: palette.text
                            background: Rectangle {
                                color: palette.base
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

            Item {
                Layout.fillWidth: true
            }

            // キャンセルボタン
            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            // 復元実行ボタン
            Button {
                id: restoreButton
                text: qsTr("Restore Selected")
                highlighted: true
                enabled: fileChangeModel.hasChanges
                onClicked: {
                    confirmRestoreDialog.open()
                }
            }
        }
    }

    // ファイル復元確認ダイアログ
    Dialog {
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
            if (!ApplicationWindow.window) return 280
            var calculated = ApplicationWindow.window.height * 0.35
            return Math.min(Math.max(calculated, 280), 650)
        }

        ColumnLayout {
            spacing: 10

            Label {
                text: qsTr("Restore selected files/directories?")
                font.bold: true
            }

            Label {
                text: qsTr("This will restore selected files and directories to snapshot #%1 state.").arg(snapshotNumber)
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
        }

        // 復元実行
        onAccepted: {
            progressDialog.currentProgress = 0
            progressDialog.totalProgress = 0
            progressDialog.currentFile = ""
            progressDialog.open()
            fileChangeModel.restoreCheckedItems()
        }
    }

    // 復元進捗表示ダイアログ
    Dialog {
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
            if (!ApplicationWindow.window) return 350
            var calculated = ApplicationWindow.window.height * 0.4
            return Math.min(Math.max(calculated, 350), 700)
        }
        standardButtons: Dialog.Cancel

        property int currentProgress: 0              // 現在の進捗
        property int totalProgress: 0                // 総ファイル数
        property string currentFile: ""              // 現在処理中のファイル

        contentItem: ColumnLayout {
            spacing: 15

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

    // 復元成功ダイアログ
    Dialog {
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
                visible: !fileChangeModel.hasChanges
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
}
