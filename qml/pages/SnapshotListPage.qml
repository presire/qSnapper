// スナップショット一覧ページ
// スナップショットの表示、作成、削除、復元機能を提供
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

Page {
    id: root
    title: qsTr("Snapshot List")

    // 選択状態管理プロパティ
    property var selectedSnapshots: ([])                       // 選択されたスナップショット番号の配列
    property int selectedCount: selectedSnapshots.length       // 選択された項目数
    property alias snapshotDetailDialog: detailDialog          // 詳細ダイアログへのエイリアス

    // ヘルパー関数: スナップショットの選択状態を切り替え
    function toggleSelection(number) {
        var index = selectedSnapshots.indexOf(number)
        if (index >= 0) {
            selectedSnapshots.splice(index, 1)
        } else {
            selectedSnapshots.push(number)
        }
        selectedSnapshots = selectedSnapshots  // バインディング更新をトリガー
    }

    // 選択状態をクリア
    function clearSelection() {
        selectedSnapshots = []
    }

    // 指定された番号のスナップショットが選択されているかチェック
    function isSelected(number) {
        return selectedSnapshots.indexOf(number) >= 0
    }

    // スナップショット一覧のデータモデル
    // D-Busサービス経由でSnapperからデータを取得
    SnapshotListModel {
        id: snapshotListModel

        // スナップショット作成成功時のシグナルハンドラ
        onSnapshotCreated: {
            statusBar.showMessage(qsTr("Snapshot created successfully"), 3000)
        }

        // スナップショット作成失敗時のシグナルハンドラ
        onSnapshotCreationFailed: function(error) {
            errorDialog.text = error
            errorDialog.open()
        }

        // スナップショット削除成功時のシグナルハンドラ
        onSnapshotDeleted: function(number) {
            statusBar.showMessage(qsTr("Snapshot #%1 deleted").arg(number), 3000)
        }

        // スナップショット削除失敗時のシグナルハンドラ
        onSnapshotDeletionFailed: function(number, error) {
            errorDialog.text = qsTr("Failed to delete snapshot #%1: %2").arg(number).arg(error)
            errorDialog.open()
        }

        // 複数スナップショット削除完了時のシグナルハンドラ
        onSnapshotsDeletionCompleted: function(successCount, failureCount) {
            root.clearSelection()
            if (failureCount === 0) {
                statusBar.showMessage(qsTr("Deleted %1 snapshot(s)").arg(successCount), 3000)
            } else {
                statusBar.showMessage(qsTr("Deletion completed: %1 succeeded, %2 failed").arg(successCount).arg(failureCount), 5000)
            }
        }

        Component.onCompleted: refresh()
    }

    // ツールバーヘッダー
    // アプリケーションタイトル、操作ボタン、テーマ切り替えを配置
    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: 5

            // アプリケーション名表示
            Label {
                text: qsTr("qSnapper")
                font.bold: true
                font.pixelSize: 18
                Layout.fillWidth: true
            }

            // 選択数表示ラベル
            Label {
                visible: root.selectedCount > 0
                text: qsTr("%1 selected").arg(root.selectedCount)
                font.pixelSize: 14
                color: palette.highlight
            }

            // 更新ボタン
            Button {
                text: qsTr("Refresh")
                icon.name: "view-refresh"
                onClicked: {
                    root.clearSelection()
                    snapshotListModel.refresh()
                }
            }

            // 選択削除ボタン
            Button {
                text: qsTr("Delete Selected")
                icon.name: "edit-delete"
                enabled: root.selectedCount > 0
                onClicked: deleteConfirmDialog.open()
            }

            // スナップショット作成ボタン
            Button {
                text: qsTr("Create Snapshot")
                icon.name: "list-add"
                onClicked: createSnapshotDialog.open()
            }

            // セパレータ
            Rectangle {
                width: 1
                height: 30
                color: palette.mid
                Layout.leftMargin: 5
                Layout.rightMargin: 5
            }

            // ダークモード切り替えラベル
            Label {
                text: qsTr("Dark Mode")
                font.pixelSize: 14
            }

            // テーマ切り替えスイッチ
            Switch {
                id: themeSwitch
                checked: ThemeManager.isDark
                onToggled: {
                    ThemeManager.themeMode = checked ? ThemeManager.Dark : ThemeManager.Light
                }
            }
        }
    }

    // メインコンテンツレイアウト
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        // スナップショット総数表示
        Label {
            text: qsTr("Total Snapshots: %1").arg(snapshotListModel.count)
            font.bold: true
        }

        // スナップショット一覧リストビュー
        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            model: snapshotListModel

            // 各スナップショット項目のデリゲート
            delegate: SnapshotItem {
                width: listView.width
                listModel: snapshotListModel
                detailDialog: root.snapshotDetailDialog
                selected: root.isSelected(number)
                onSelectionToggled: root.toggleSelection(number)
            }

            ScrollBar.vertical: ScrollBar {}

            // スナップショットが存在しない場合の表示
            Label {
                anchors.centerIn: parent
                visible: snapshotListModel.count === 0
                text: qsTr("No snapshots available")
                font.pixelSize: 16
                color: palette.placeholderText
            }
        }
    }

    // フッターステータスバー
    // ステータスメッセージと「About」ボタンを配置
    footer: Rectangle {
        id: statusBar
        height: 30
        color: palette.window

        property alias text: statusLabel.text

        // 一時的なステータスメッセージを表示
        function showMessage(message, timeout) {
            statusLabel.text = message
            statusTimer.interval = timeout
            statusTimer.restart()
        }

        // ステータスメッセージ表示ラベル
        Label {
            id: statusLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 10
        }

        // qSnapperについてボタン
        Button {
            id: aboutqSnapperButton
            text: qsTr("About qSnapper")
            flat: true
            anchors.right: aboutQtButton.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 10
            onClicked: aboutqSnapperDialog.open()
        }

        // Qtライセンスボタン
        Button {
            id: aboutQtButton
            text: qsTr("Qt License")
            flat: true
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 10
            onClicked: aboutQtDialog.open()
        }

        // ステータスメッセージ自動クリアタイマー
        Timer {
            id: statusTimer
            interval: 3000
            onTriggered: statusLabel.text = ""
        }
    }

    // スナップショット作成ダイアログ
    Dialog {
        id: createSnapshotDialog
        title: qsTr("Create Snapshot")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: {
            if (!ApplicationWindow.window) return 600
            var calculated = ApplicationWindow.window.width * 0.5
            return Math.min(Math.max(calculated, 600), 1200)
        }
        height: {
            if (!ApplicationWindow.window) return 400
            var calculated = ApplicationWindow.window.height * 0.5
            return Math.min(Math.max(calculated, 400), 800)
        }

        ColumnLayout {
            width: parent.width
            spacing: 20
            anchors.top: parent.top
            anchors.topMargin: 20

            // 説明ラベル
            Label {
                text: qsTr("Description:")
            }

            // 説明入力フィールド
            TextField {
                id: descriptionField
                Layout.fillWidth: true
                placeholderText: qsTr("Enter snapshot description")
            }

            // スナップショットタイプ選択コンボボックス
            ComboBox {
                id: snapshotTypeCombo
                Layout.fillWidth: true
                model: [qsTr("Single Snapshot"), qsTr("Pre Snapshot")]
                currentIndex: 0
            }

            // Preスナップショットの説明ラベル
            Label {
                visible: snapshotTypeCombo.currentIndex === 1
                text: qsTr("Pre snapshot can be paired with post snapshot later")
                font.italic: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        // OKボタン押下時の処理
        onAccepted: {
            if (descriptionField.text.trim() === "") {
                errorDialog.text = qsTr("Please enter a description")
                errorDialog.open()
                return
            }

            if (snapshotTypeCombo.currentIndex === 0) {
                snapshotListModel.createSingleSnapshot(descriptionField.text)
            } else {
                snapshotListModel.createPreSnapshot(descriptionField.text)
            }

            descriptionField.text = ""
        }

        // キャンセルボタン押下時の処理
        onRejected: {
            descriptionField.text = ""
        }
    }

    // 削除確認ダイアログ
    Dialog {
        id: deleteConfirmDialog
        title: qsTr("Confirmation")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: {
            if (!ApplicationWindow.window) return 500
            var calculated = ApplicationWindow.window.width * 0.4
            return Math.min(Math.max(calculated, 500), 800)
        }
        height: {
            if (!ApplicationWindow.window) return 250
            var calculated = ApplicationWindow.window.height * 0.3
            return Math.min(Math.max(calculated, 250), 600)
        }

        ColumnLayout {
            width: parent.width
            spacing: 15

            Label {
                text: root.selectedCount === 1
                    ? qsTr("Delete selected snapshot?")
                    : qsTr("Delete %1 snapshots?").arg(root.selectedCount)
                font.bold: true
                font.pixelSize: 14
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("This operation cannot be undone.")
                color: ThemeManager.errorColor
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Note: This operation requires administrator privileges.")
                font.italic: true
                font.pixelSize: 12
                color: palette.placeholderText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        // 削除実行
        onAccepted: {
            snapshotListModel.deleteSnapshots(root.selectedSnapshots)
        }
    }

    // エラーダイアログ
    Dialog {
        id: errorDialog
        title: qsTr("Error")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok
        width: {
            if (!ApplicationWindow.window) return 450
            var calculated = ApplicationWindow.window.width * 0.35
            return Math.min(Math.max(calculated, 450), 700)
        }
        height: {
            if (!ApplicationWindow.window) return 200
            var calculated = ApplicationWindow.window.height * 0.25
            return Math.min(Math.max(calculated, 200), 500)
        }

        property alias text: errorLabel.text

        Label {
            id: errorLabel
            wrapMode: Text.WordWrap
        }
    }

    // qSnapperについてダイアログ
    AboutqSnapperDialog {
        id: aboutqSnapperDialog
    }

    // Qtライセンスダイアログ
    AboutQtDialog {
        id: aboutQtDialog
    }

    // スナップショット詳細ダイアログ
    SnapshotDetailDialog {
        id: detailDialog
        snapshotListModel: snapshotListModel
        anchors.centerIn: Overlay.overlay
    }
}
